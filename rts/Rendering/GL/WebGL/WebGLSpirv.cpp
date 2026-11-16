/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Desktop GLSL -> SPIR-V (glslang) -> GLSL ES 3.00 (SPIRV-Cross), see WebGLShaderTranslate.h.
// Only built into engine-web when both libraries were found (RECOIL_WEB_GLSLANG).

#include "WebGLShaderTranslate.h"

#include <cstdlib>
#include <cstring>

#ifdef RECOIL_WEB_GLSLANG
#include <exception>
#include <set>

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross/spirv_glsl.hpp>
#endif

namespace {
	int numOk = 0;
	int numFailed = 0;
}

void WebGLCompat::GetCrossCompileStats(int& ok, int& failed) { ok = numOk; failed = numFailed; }

#ifndef RECOIL_WEB_GLSLANG

bool WebGLCompat::CrossCompileAvailable() { return false; }
bool WebGLCompat::PreprocessGlsl(const std::string&, std::string&) { return false; }
bool WebGLCompat::CrossCompile(bool, const std::string&, CrossCompiled& out) { out.log = "built without glslang"; return false; }

#else

bool WebGLCompat::CrossCompileAvailable()
{
	// Prefer cross-compilation when built in. WebGL2 LuaVAOs support the basic
	// instanced draws needed after BAR's scum shader compiles. Set mode 0 for
	// the text fallback; mode 2 compiles but discards the output.
	static const bool enabled = [] {
		const char* env = std::getenv("RECOIL_WEB_GLSLANG");
		return env == nullptr || std::strcmp(env, "0") != 0;
	}();
	return enabled;
}

static bool InitGlslang()
{
	static const bool initialized = glslang::InitializeProcess();
	return initialized;
}

bool WebGLCompat::PreprocessGlsl(const std::string& src, std::string& out)
{
	if (!InitGlslang())
		return false;
	// the stage only matters for predefined macros; none of the engine's shaders test it
	glslang::TShader shader(EShLangVertex);
	const char* text = src.c_str();
	shader.setStrings(&text, 1);
	glslang::TShader::ForbidIncluder includer;
	return shader.preprocess(GetDefaultResources(), 110, ENoProfile, false, false, EShMsgDefault, &out, includer);
}

bool WebGLCompat::CrossCompile(bool isVertex, const std::string& desktopSrc, CrossCompiled& out)
{
	if (!InitGlslang()) {
		out.log = "glslang::InitializeProcess failed";
		numFailed++;
		return false;
	}

	const EShLanguage stage = isVertex? EShLangVertex: EShLangFragment;
	const EShMessages messages = EShMessages(EShMsgSpvRules);

	// --- GLSL -> SPIR-V, OpenGL semantics (ARB_gl_spirv); locations and bindings the source
	// leaves open are assigned automatically and removed again below
	glslang::TShader shader(stage);
	const char* text = desktopSrc.c_str();
	shader.setStrings(&text, 1);
	shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientOpenGL, 100);
	shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
	shader.setAutoMapLocations(true);
	shader.setAutoMapBindings(true);

	if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
		out.log = std::string("glslang: ") + shader.getInfoLog();
		numFailed++;
		return false;
	}

	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(messages)) {
		out.log = std::string("glslang link: ") + program.getInfoLog();
		numFailed++;
		return false;
	}

	// what the source placed explicitly, before mapIO fills in the rest
	std::set<std::string> explicitInputs;
	if (program.buildReflection(EShReflectionDefault)) {
		for (int i = 0; i < program.getNumUniformVariables(); i++) {
			const glslang::TObjectReflection& u = program.getUniform(i);
			if (u.getBinding() >= 0)
				out.samplerBindings.emplace_back(u.name, u.getBinding());
		}
		for (int i = 0; i < program.getNumUniformBlocks(); i++) {
			const glslang::TObjectReflection& b = program.getUniformBlock(i);
			if (b.getBinding() >= 0)
				out.blockBindings.emplace_back(b.name, b.getBinding());
		}
		for (int i = 0; i < program.getNumPipeInputs(); i++) {
			const glslang::TObjectReflection& in = program.getPipeInput(i);
			if (in.layoutLocation() < 64)
				explicitInputs.insert(in.name);
		}
	}

	if (!program.mapIO()) {
		out.log = std::string("glslang mapIO: ") + program.getInfoLog();
		numFailed++;
		return false;
	}

	std::vector<uint32_t> spirv;
	glslang::SpvOptions spvOptions;
	spvOptions.disableOptimizer = true;
	spvOptions.validate = false;
	glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &spvOptions);

	// --- SPIR-V -> GLSL ES 3.00
	try {
		spirv_cross::CompilerGLSL compiler(std::move(spirv));

		spirv_cross::CompilerGLSL::Options opts = compiler.get_common_options();
		opts.version = 300;
		opts.es = true;
		opts.vertex.fixup_clipspace = false;
		opts.vertex.flip_vert_y = false;
		opts.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
		opts.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
		compiler.set_common_options(opts);

		const spirv_cross::ShaderResources res = compiler.get_shader_resources();

		// vertex inputs keep only the locations the source gave them: the others (the
		// fixed-function rcl_* attributes among them) are placed with glBindAttribLocation
		for (const spirv_cross::Resource& r: res.stage_inputs) {
			if (!isVertex || explicitInputs.count(compiler.get_name(r.id)) == 0)
				compiler.unset_decoration(r.id, spv::DecorationLocation);
		}
		// varyings match by name in GLSL ES 3.00
		if (isVertex) {
			for (const spirv_cross::Resource& r: res.stage_outputs)
				compiler.unset_decoration(r.id, spv::DecorationLocation);
		}
		for (const spirv_cross::Resource& r: res.gl_plain_uniforms)
			compiler.unset_decoration(r.id, spv::DecorationLocation);
		for (const auto* list: {&res.sampled_images, &res.separate_images, &res.uniform_buffers}) {
			for (const spirv_cross::Resource& r: *list) {
				compiler.unset_decoration(r.id, spv::DecorationBinding);
				compiler.unset_decoration(r.id, spv::DecorationDescriptorSet);
			}
		}

		out.source = compiler.compile();
	} catch (const std::exception& e) {
		out.log = std::string("SPIRV-Cross: ") + e.what();
		numFailed++;
		return false;
	}

	numOk++;
	return true;
}

#endif
