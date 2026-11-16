/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "WebGLCompat.h"
#include "WebGLInternal.h"
#include "WebGLShaderTranslate.h"

#include <glad/glad.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "System/Log/ILog.h"

extern "C" void* emscripten_GetProcAddress(const char* name);
extern "C" void* emscripten_webgl2_get_proc_address(const char* name);
extern "C" void emscripten_glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data);

namespace {
	constexpr int MAX_GL_FUNCS = 4096;
	std::atomic<uint32_t> hitCounts[MAX_GL_FUNCS];
	uint32_t reportedCounts[MAX_GL_FUNCS];
	bool stubbed[MAX_GL_FUNCS];

	void LogFirstHit(int idx);
	inline void Hit(int idx) {
		if (hitCounts[idx].fetch_add(1, std::memory_order_relaxed) == 0)
			LogFirstHit(idx);
	}

	void GLCheckAfter(int idx);

	#include "WebGLStubs.gen.inc"

	static_assert(NUM_GL_FUNCS <= MAX_GL_FUNCS, "raise MAX_GL_FUNCS");

	// one line the first time each unsupported entry point is called, so the log shows
	// where in the engine's startup or frame it happened
	void LogFirstHit(int idx) { LOG("[WebGLCompat] first call of unsupported %s (no-op)", glFuncNames[idx]); }


	// ---- RECOIL_WEB_GLCHECK=1 (debugging aid): every non-stubbed entry point is wrapped so
	// that glGetError is read after it. Errors are attributed to the call that raised them
	// and logged with a call stack (wasm function indices; spring-web.js.symbols names them).
	// The first error is kept and returned by the next glGetError, so the engine's own error
	// checks still see it. Each glGetError is a synchronous round trip: expect slow frames.
	PFNGLGETERRORPROC real_glGetError = nullptr;
	GLenum heldError = GL_NO_ERROR;
	uint32_t glCheckErrors[MAX_GL_FUNCS][4]; // per entry point: INVALID_ENUM/VALUE/OPERATION, other
	uint32_t glCheckReported[MAX_GL_FUNCS][4];
	int glCheckStacksLeft = 64;

	void GLCheckAfter(int idx) {
		const GLenum err = real_glGetError();
		if (err == GL_NO_ERROR)
			return;
		if (heldError == GL_NO_ERROR)
			heldError = err;

		const int slot = (err >= GL_INVALID_ENUM && err <= GL_INVALID_OPERATION)? int(err - GL_INVALID_ENUM): 3;
		// a call stack for the first two errors of each kind per entry point
		if (glCheckErrors[idx][slot]++ >= 2 || glCheckStacksLeft <= 0) {
			return;
		}
		glCheckStacksLeft--;
		static char stack[4096];
		emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_NO_PATHS, stack, sizeof(stack));
		LOG_L(L_WARNING, "[WebGLCompat] glcheck: %s raised GL error 0x%x; stack:\n%s", glFuncNames[idx], err, stack);
	}

	GLenum APIENTRY glcheck_glGetError() {
		const GLenum held = heldError;
		heldError = GL_NO_ERROR;
		return (held != GL_NO_ERROR)? held: real_glGetError();
	}

	void LogGLCheckStats(bool final) {
		if (real_glGetError == nullptr)
			return;
		std::string line;
		for (int i = 0; i < NUM_GL_FUNCS; i++) {
			for (int s = 0; s < 4; s++) {
				const uint32_t n = glCheckErrors[i][s];
				if (n == glCheckReported[i][s] && !(final && n > 0))
					continue;
				line += glFuncNames[i];
				line += (s < 3)? ":0x" + std::to_string(500 + s): std::string(":other");
				line += '=';
				line += std::to_string(final? n: n - glCheckReported[i][s]);
				line += ' ';
				glCheckReported[i][s] = n;
			}
		}
		if (!line.empty())
			LOG("[WebGLCompat] glcheck: GL errors %s: %s", final? "in total": "since last report", line.c_str());
	}

	// real WebGL2 entry points behind the overrides below
	PFNGLGETSTRINGPROC   real_glGetString   = nullptr;
	PFNGLGETSTRINGIPROC  real_glGetStringi  = nullptr;
	PFNGLGETINTEGERVPROC real_glGetIntegerv = nullptr;

	std::vector<std::string> extensions;
	std::string versionStr;
	std::string glslVersionStr;

	bool HaveRealExt(const char* name) {
		GLint n = 0;
		real_glGetIntegerv(GL_NUM_EXTENSIONS, &n);
		for (GLint i = 0; i < n; i++) {
			const char* e = reinterpret_cast<const char*>(real_glGetStringi(GL_EXTENSIONS, i));
			if (e != nullptr && std::strcmp(e, name) == 0)
				return true;
		}
		return false;
	}

	// what the engine sees: the desktop extensions whose functionality is core in
	// GLES 3.0 (or emulated by this layer), plus the real WebGL extensions
	void BuildExtensionList() {
		static const char* const coreInES3[] = {
			"GL_ARB_multitexture",
			"GL_ARB_texture_env_combine",
			"GL_ARB_texture_compression",
			"GL_ARB_texture_non_power_of_two",
			"GL_ARB_framebuffer_object",
			"GL_EXT_framebuffer_object",
			"GL_EXT_framebuffer_blit",
			"GL_EXT_framebuffer_multisample",
			"GL_ARB_vertex_shader",
			"GL_ARB_fragment_shader",
			"GL_ARB_shader_objects",
			"GL_ARB_shading_language_100",
			"GL_ARB_vertex_buffer_object",
			"GL_ARB_vertex_array_object",
			"GL_ARB_pixel_buffer_object",
			"GL_ARB_map_buffer_range",
			"GL_ARB_copy_buffer",
			"GL_ARB_uniform_buffer_object",
			"GL_ARB_instanced_arrays",
			"GL_ARB_draw_instanced",
			"GL_ARB_draw_buffers",
			"GL_ARB_explicit_attrib_location",
			"GL_ARB_depth_texture",
			"GL_ARB_shadow",
			"GL_ARB_texture_float",
			"GL_ARB_half_float_pixel",
			"GL_ARB_half_float_vertex",
			"GL_ARB_texture_rg",
			"GL_ARB_texture_storage",
			"GL_ARB_sampler_objects",
			"GL_ARB_sync",
			"GL_ARB_occlusion_query",
			"GL_ARB_occlusion_query2",
			"GL_ARB_transform_feedback2",
			"GL_ARB_seamless_cube_map",
			"GL_ARB_texture_cube_map",
			"GL_EXT_texture_array",
			"GL_EXT_texture_edge_clamp",
			"GL_EXT_blend_minmax",
			"GL_EXT_blend_subtract",
			"GL_EXT_blend_func_separate",
			"GL_EXT_blend_equation_separate",
			"GL_EXT_stencil_two_side",
			"GL_EXT_packed_depth_stencil",
			"GL_EXT_texture_sRGB",
			"GL_ARB_ES3_compatibility",
			"GL_ARB_ES2_compatibility",
		};
		extensions.clear();
		for (const char* e: coreInES3)
			extensions.emplace_back(e);

		GLint n = 0;
		real_glGetIntegerv(GL_NUM_EXTENSIONS, &n);
		for (GLint i = 0; i < n; i++) {
			const char* e = reinterpret_cast<const char*>(real_glGetStringi(GL_EXTENSIONS, i));
			if (e != nullptr)
				extensions.emplace_back(e);
		}
		// WebGL names for desktop extensions the engine looks up by their desktop name
		if (HaveRealExt("GL_WEBGL_compressed_texture_s3tc") || HaveRealExt("GL_EXT_texture_compression_s3tc"))
			extensions.emplace_back("GL_EXT_texture_compression_s3tc");
		if (HaveRealExt("GL_EXT_color_buffer_float"))
			extensions.emplace_back("GL_ARB_color_buffer_float");
		// layout(binding = N) on samplers and uniform blocks: the cross-compiled shaders
		// return those bindings and the link applies them (glUniform1i/glUniformBlockBinding).
		// The engine's UniformConstants UBOs (camera matrices and frame parameters that BAR's
		// GL4 shaders read at bindings 0 and 1) require it; without them nothing is bound
		// at those slots and WebGL rejects every draw of such a program.
		if (WebGLCompat::CrossCompileAvailable())
			extensions.emplace_back("GL_ARB_shading_language_420pack");

		std::sort(extensions.begin(), extensions.end());
		extensions.erase(std::unique(extensions.begin(), extensions.end()), extensions.end());
	}

	const GLubyte* APIENTRY compat_glGetString(GLenum name) {
		switch (name) {
			case GL_VERSION: return reinterpret_cast<const GLubyte*>(versionStr.c_str());
			case GL_SHADING_LANGUAGE_VERSION: return reinterpret_cast<const GLubyte*>(glslVersionStr.c_str());
			case GL_EXTENSIONS: return reinterpret_cast<const GLubyte*>("");
			default: return real_glGetString(name);
		}
	}

	const GLubyte* APIENTRY compat_glGetStringi(GLenum name, GLuint index) {
		if (name == GL_EXTENSIONS)
			return (index < extensions.size())? reinterpret_cast<const GLubyte*>(extensions[index].c_str()): nullptr;
		return real_glGetStringi(name, index);
	}

	void APIENTRY compat_glGetIntegerv(GLenum pname, GLint* data) {
		switch (pname) {
			case GL_NUM_EXTENSIONS: { *data = static_cast<GLint>(extensions.size()); } return;
			// desktop-only queries
			case GL_MAX_TEXTURE_COORDS: { real_glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, data); *data = std::min(*data, 8); } return;
			case GL_MAX_TEXTURE_UNITS: { *data = 8; } return;
			case GL_MAX_VARYING_FLOATS: { real_glGetIntegerv(GL_MAX_VARYING_VECTORS, data); *data *= 4; } return;
			case GL_MAX_CLIP_PLANES: { *data = 6; } return;
			case GL_MAX_LIGHTS: { *data = 8; } return;
			case GL_CONTEXT_PROFILE_MASK: { *data = GL_CONTEXT_COMPATIBILITY_PROFILE_BIT; } return;
			case GL_CONTEXT_FLAGS: { *data = 0; } return;
			case GL_MAJOR_VERSION: { *data = 3; } return;
			case GL_MINOR_VERSION: { *data = 0; } return;
			default: break;
		}
		real_glGetIntegerv(pname, data);
	}

	PFNGLGETQUERYOBJECTUIVPROC real_glGetQueryObjectuiv = nullptr;

	// WebGL2 only has the unsigned variant. The engine also polls QUERY_RESULT_AVAILABLE
	// in a loop for its (unsupported here) timestamp queries, so a query that was never
	// issued must report "available" with a zero result instead of leaving the output alone.
	void APIENTRY compat_glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
		GLuint v = (pname == GL_QUERY_RESULT_AVAILABLE)? 1: 0;
		real_glGetQueryObjectuiv(id, pname, &v);
		*params = static_cast<GLint>(v);
	}

	// WebGL2 has no glGetFramebufferParameteriv; the engine asks it only for the sample
	// counts of the bound framebuffers (FBO::Blit), which glGetIntegerv knows
	void APIENTRY compat_glGetFramebufferParameteriv(GLenum target, GLenum pname, GLint* params) {
		*params = 0;
		if (pname == GL_SAMPLES || pname == GL_SAMPLE_BUFFERS)
			real_glGetIntegerv(pname, params);
	}

	void* LoadProc(const char* name) {
		if (std::strcmp(name, "glGetString") == 0) return reinterpret_cast<void*>(compat_glGetString);
		if (std::strcmp(name, "glGetFramebufferParameteriv") == 0) return reinterpret_cast<void*>(compat_glGetFramebufferParameteriv);
		// in Emscripten's WebGL2 library but missing from its proc-address table
		if (std::strcmp(name, "glGetBufferSubData") == 0) return reinterpret_cast<void*>(emscripten_glGetBufferSubData);
		if (std::strcmp(name, "glGetStringi") == 0) return reinterpret_cast<void*>(compat_glGetStringi);
		if (std::strcmp(name, "glGetIntegerv") == 0) return reinterpret_cast<void*>(compat_glGetIntegerv);
		if (std::strcmp(name, "glGetQueryObjectiv") == 0) return reinterpret_cast<void*>(compat_glGetQueryObjectiv);

		// emscripten_GetProcAddress strips EXT/ARB/OES suffixes and looks in its WebGL1
		// extension table before the WebGL2 core one, so e.g. glGenQueries resolves to the
		// EXT_disjoint_timer_query (WebGL1) implementation, which throws on a WebGL2 context.
		// Ask for the WebGL2 core function first.
		if (void* ptr = emscripten_webgl2_get_proc_address(name); ptr != nullptr)
			return ptr;

		// WebGL1-extension-only query entry points with no WebGL2 counterpart: leave them
		// null (stubbed) rather than resolving them to functions that throw
		for (const char* n: {"glQueryCounter", "glGetQueryObjecti64v", "glGetQueryObjectui64v"}) {
			if (std::strcmp(name, n) == 0)
				return nullptr;
		}

		return emscripten_GetProcAddress(name);
	}
}


extern "C" char __global_base;
extern "C" char __data_end;

void WebGLCompat::Canary(const char* where)
{
	// musl's printf sign/prefix table; it was among the constants seen overwritten
	// the start of static data is read-only constants (string literals), which wild
	// writes through near-null pointers hit: keep a copy and compare
	constexpr size_t WATCH_SIZE = 64 * 1024;
	static char* snapshot = nullptr;
	static const char* lastGood = "(start)";
	static bool dead = false;
	const char* base = &__global_base;

	if (dead)
		return;
	if (snapshot == nullptr) {
		snapshot = static_cast<char*>(std::malloc(WATCH_SIZE));
		std::memcpy(snapshot, base, WATCH_SIZE);
		LOG("[WebGLCompat::Canary] watching %p..%p (static data ends at %p)", base, base + WATCH_SIZE, &__data_end);
	}
	if (std::memcmp(base, snapshot, WATCH_SIZE) == 0) {
		lastGood = where;
		return;
	}
	dead = true;

	size_t first = 0, last = 0, count = 0;
	for (size_t i = 0; i < WATCH_SIZE; i++) {
		if (base[i] == snapshot[i])
			continue;
		if (count++ == 0)
			first = i;
		last = i;
	}
	char hex[3 * 32 + 1] = {0};
	for (size_t i = 0; i < 32 && (first + i) < WATCH_SIZE; i++)
		snprintf(hex + i * 3, 4, "%02x ", static_cast<unsigned char>(base[first + i]));
	LOG_L(L_ERROR, "[WebGLCompat::Canary] static data overwritten: %d bytes changed in %p..%p; noticed at \"%s\", last intact at \"%s\"; new bytes at %p: %s",
		int(count), base + first, base + last, where, lastGood, base + first, hex);
}


void* WebGLCompat::CreateContext()
{
	EmscriptenWebGLContextAttributes attrs;
	emscripten_webgl_init_context_attributes(&attrs);

	attrs.majorVersion = 2;
	attrs.minorVersion = 0;
	// RGBA like a desktop default framebuffer: WebGL only copies (glCopyTexSubImage2D, e.g.
	// BAR's screen copies for CAS and blur) into formats whose components the source has,
	// so an RGB canvas rejects copies into RGBA textures. The engine writes arbitrary alpha,
	// which the page would blend with whatever is behind the canvas: SwapWindow sets it to 1.
	attrs.alpha = true;
	attrs.depth = true;
	attrs.stencil = true;
	attrs.antialias = false;
	attrs.premultipliedAlpha = false;
	attrs.preserveDrawingBuffer = false;
	attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
	attrs.enableExtensionsByDefault = true;
	attrs.explicitSwapControl = false;

	const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle = emscripten_webgl_create_context("#canvas", &attrs);

	if (handle <= 0) {
		LOG_L(L_ERROR, "[WebGLCompat::%s] emscripten_webgl_create_context(\"#canvas\") failed with code %d", __func__, int(handle));
		return nullptr;
	}
	if (emscripten_webgl_make_context_current(handle) != EMSCRIPTEN_RESULT_SUCCESS) {
		LOG_L(L_ERROR, "[WebGLCompat::%s] could not make the WebGL2 context current", __func__);
		emscripten_webgl_destroy_context(handle);
		return nullptr;
	}

	// user clip planes (unit build stages, Lua gl.ClipPlane); without it they are ignored
	const bool clipDistance = emscripten_webgl_enable_extension(handle, "WEBGL_clip_cull_distance");
	SetClipDistanceSupport(clipDistance);
	LOG("[WebGLCompat::%s] WEBGL_clip_cull_distance: %s", __func__, clipDistance? "enabled (user clip planes)": "unavailable (user clip planes ignored)");

	return reinterpret_cast<void*>(handle);
}

void WebGLCompat::DeleteContext(void* ctx)
{
	if (ctx != nullptr)
		emscripten_webgl_destroy_context(reinterpret_cast<EMSCRIPTEN_WEBGL_CONTEXT_HANDLE>(ctx));
}

int WebGLCompat::MakeCurrent(void* ctx)
{
	return (emscripten_webgl_make_context_current(reinterpret_cast<EMSCRIPTEN_WEBGL_CONTEXT_HANDLE>(ctx)) == EMSCRIPTEN_RESULT_SUCCESS)? 0: -1;
}

// alpha of the default framebuffer to 1, so the canvas composites as opaque
static void ForceOpaqueAlpha()
{
	GLint drawFbo = 0;
	GLboolean mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
	GLfloat clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	real_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
	glGetBooleanv(GL_COLOR_WRITEMASK, mask);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
	const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	if (scissor) glDisable(GL_SCISSOR_TEST);
	glClear(GL_COLOR_BUFFER_BIT);

	if (scissor) glEnable(GL_SCISSOR_TEST);
	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	glColorMask(mask[0], mask[1], mask[2], mask[3]);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFbo));
}

void WebGLCompat::SwapWindow()
{
	ForceOpaqueAlpha();

	// nothing to present explicitly; use the frame boundary to report what is still missing
	static double lastReportMs = 0.0;
	const double nowMs = emscripten_get_now();

	// debugging aid: with RECOIL_WEB_CAPTURE=<seconds> in the environment, dump the default
	// framebuffer every <seconds> to <cwd>/capture.rgba (int32 width, int32 height, RGBA8
	// rows bottom-up) so a page that cannot be looked at (hidden tab) can still be checked
	static double captureIntervalMs = -1.0;
	static double lastCaptureMs = 0.0;
	if (captureIntervalMs < 0.0) {
		const char* env = getenv("RECOIL_WEB_CAPTURE");
		captureIntervalMs = (env != nullptr)? std::max(0.0, atof(env) * 1000.0): 0.0;
	}
	if (captureIntervalMs > 0.0 && (nowMs - lastCaptureMs) > captureIntervalMs) {
		lastCaptureMs = nowMs;
		// RECOIL_WEB_TRACE=1: also log the next 60 immediate-mode draws (matrices, state)
		static const bool trace = (getenv("RECOIL_WEB_TRACE") != nullptr);
		if (trace)
			traceDraws = 60;
		int size[2] = {0, 0};
		emscripten_webgl_get_drawing_buffer_size(emscripten_webgl_get_current_context(), &size[0], &size[1]);
		if (size[0] > 0 && size[1] > 0) {
			std::vector<uint8_t> pixels(size_t(size[0]) * size[1] * 4);
			GLint prevReadFbo = 0;
			GLint prevPackBuf = 0;
			{
				// state left at the end of the frame, for "why is the picture missing X"
				GLint drawFbo = 0, vp[4] = {0, 0, 0, 0}, sc[4] = {0, 0, 0, 0};
				GLboolean cm[4] = {0, 0, 0, 0};
				real_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
				real_glGetIntegerv(GL_VIEWPORT, vp);
				real_glGetIntegerv(GL_SCISSOR_BOX, sc);
				glGetBooleanv(GL_COLOR_WRITEMASK, cm);
				GLint prog = 0, bsrc = 0, bdst = 0, depthFunc = 0, err = 0;
				real_glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
				real_glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc);
				real_glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
				real_glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
				err = GLint(glGetError());
				LOG("[WebGLCompat::%s] capture: drawFbo=%d viewport=%d,%d,%d,%d scissorTest=%d box=%d,%d,%d,%d colorMask=%d%d%d%d"
					" depth=%d(func 0x%x) stencil=%d blend=%d(0x%x,0x%x) cull=%d tex2D=%d alphaTest=%d program=%d glError=0x%x",
					__func__, drawFbo, vp[0], vp[1], vp[2], vp[3], int(glIsEnabled(GL_SCISSOR_TEST)), sc[0], sc[1], sc[2], sc[3], cm[0], cm[1], cm[2], cm[3],
					int(glIsEnabled(GL_DEPTH_TEST)), depthFunc, int(glIsEnabled(GL_STENCIL_TEST)), int(glIsEnabled(GL_BLEND)), bsrc, bdst,
					int(glIsEnabled(GL_CULL_FACE)), int(glIsEnabled(GL_TEXTURE_2D)), int(glIsEnabled(GL_ALPHA_TEST)), prog, err);
			}
			real_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
			real_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPackBuf);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			glReadPixels(0, 0, size[0], size[1], GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
			glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(prevPackBuf));
			glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(prevReadFbo));
			if (FILE* f = fopen("capture.rgba.tmp", "wb"); f != nullptr) {
				fwrite(size, sizeof(int), 2, f);
				fwrite(pixels.data(), 1, pixels.size(), f);
				fclose(f);
				rename("capture.rgba.tmp", "capture.rgba");
			}
		}
	}

	if ((nowMs - lastReportMs) > 20000.0) {
		lastReportMs = nowMs;
		LogStubStats(false);
	}
}


bool WebGLCompat::Load()
{
	real_glGetString   = reinterpret_cast<PFNGLGETSTRINGPROC  >(emscripten_GetProcAddress("glGetString"));
	real_glGetStringi  = reinterpret_cast<PFNGLGETSTRINGIPROC >(emscripten_GetProcAddress("glGetStringi"));
	real_glGetIntegerv = reinterpret_cast<PFNGLGETINTEGERVPROC>(emscripten_GetProcAddress("glGetIntegerv"));
	real_glGetQueryObjectuiv = reinterpret_cast<PFNGLGETQUERYOBJECTUIVPROC>(emscripten_webgl2_get_proc_address("glGetQueryObjectuiv"));

	if (real_glGetString == nullptr || real_glGetStringi == nullptr || real_glGetIntegerv == nullptr) {
		LOG_L(L_ERROR, "[WebGLCompat::%s] Emscripten GL entry points missing (not linked with WebGL2?)", __func__);
		return false;
	}

	const char* realVersion = reinterpret_cast<const char*>(real_glGetString(GL_VERSION));
	const char* realGLSL    = reinterpret_cast<const char*>(real_glGetString(GL_SHADING_LANGUAGE_VERSION));

	if (realVersion == nullptr) {
		LOG_L(L_ERROR, "[WebGLCompat::%s] no current WebGL context", __func__);
		return false;
	}

	// desktop-style version strings; the real ones follow in parentheses for the log
	versionStr     = std::string("3.0 WebGL2-compat (") + realVersion + ")";
	glslVersionStr = std::string("1.30 WebGL2-compat (") + ((realGLSL != nullptr)? realGLSL: "?") + ")";

	BuildExtensionList();

	if (!gladLoadGLLoader(LoadProc))
		return false;

	FillNullEntryPoints();
	InstallStateEmulation();
	InstallObjectEmulation();
	InstallImmediateEmulation();

	if (const char* env = getenv("RECOIL_WEB_GLCHECK"); env != nullptr && atoi(env) != 0) {
		InstallErrorChecks();
		real_glGetError = glad_glGetError;
		glad_glGetError = glcheck_glGetError;
		LOG("[WebGLCompat::%s] RECOIL_WEB_GLCHECK: glGetError is checked after every GL call", __func__);
	}

	int numStubbed = 0;
	for (int i = 0; i < NUM_GL_FUNCS; i++)
		numStubbed += stubbed[i];

	LOG("[WebGLCompat::%s] %d of %d GL entry points are native WebGL2, %d stubbed; %d extensions reported",
		__func__, NUM_GL_FUNCS - numStubbed, NUM_GL_FUNCS, numStubbed, int(extensions.size()));
	return true;
}


void WebGLCompat::LogStubStats(bool final)
{
	LogGLCheckStats(final);
	{
		static uint32_t lastRecorded = ~0u, lastFailed = ~0u, lastSkipped = ~0u;
		uint32_t failed = 0;
		const uint32_t recorded = GetListDrawStats(&failed);
		const uint32_t skipped = GetSkippedDrawCount();
		if (final || recorded != lastRecorded || failed != lastFailed || skipped != lastSkipped) {
			LOG("[WebGLCompat::%s] draws recorded into display lists: %u (%u with missing data); GL_QUADS element draws dropped: %u",
				__func__, recorded, failed, skipped);
			lastRecorded = recorded; lastFailed = failed; lastSkipped = skipped;
		}
	}
	{
		static int lastOk = -1, lastFailed = -1;
		int ok = 0, failed = 0;
		GetCrossCompileStats(ok, failed);
		if (CrossCompileAvailable() && (final || ok != lastOk || failed != lastFailed)) {
			LOG("[WebGLCompat::%s] shaders cross-compiled with glslang + SPIRV-Cross: %d; fell back to the text translation: %d",
				__func__, ok, failed);
			lastOk = ok; lastFailed = failed;
		}
	}

	std::vector<std::pair<uint32_t, int>> hits;
	for (int i = 0; i < NUM_GL_FUNCS; i++) {
		const uint32_t n = hitCounts[i].load(std::memory_order_relaxed);
		if (final) {
			if (n > 0)
				hits.emplace_back(n, i);
			continue;
		}
		if (n == reportedCounts[i])
			continue;
		hits.emplace_back(n - reportedCounts[i], i);
		reportedCounts[i] = n;
	}
	if (hits.empty())
		return;

	std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

	std::string line;
	for (const auto& [n, i]: hits) {
		line += glFuncNames[i];
		line += ':';
		line += std::to_string(n);
		line += ' ';
	}
	LOG("[WebGLCompat::%s] unsupported GL calls %s(%d functions): %s", __func__, final? "in total ": "since last report ", int(hits.size()), line.c_str());
}
