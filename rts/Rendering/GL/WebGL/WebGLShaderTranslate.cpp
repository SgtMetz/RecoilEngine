/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "WebGLShaderTranslate.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {
	bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

	// position of the next occurrence of identifier `id` (whole token) at or after `from`
	size_t FindIdent(const std::string& s, std::string_view id, size_t from = 0) {
		for (size_t pos = s.find(id, from); pos != std::string::npos; pos = s.find(id, pos + 1)) {
			const bool startOk = (pos == 0) || !IsIdentChar(s[pos - 1]);
			const bool endOk = (pos + id.size() >= s.size()) || !IsIdentChar(s[pos + id.size()]);
			if (startOk && endOk)
				return pos;
		}
		return std::string::npos;
	}

	bool HasIdent(const std::string& s, std::string_view id) { return FindIdent(s, id) != std::string::npos; }

	// replace every whole-token occurrence of `from`; returns the number of replacements
	int ReplaceIdent(std::string& s, std::string_view from, std::string_view to) {
		int n = 0;
		for (size_t pos = FindIdent(s, from); pos != std::string::npos; pos = FindIdent(s, from, pos + to.size())) {
			s.replace(pos, from.size(), to);
			n++;
		}
		return n;
	}

	size_t SkipSpace(const std::string& s, size_t pos) {
		while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
			pos++;
		return pos;
	}

	std::string_view ReadIdent(const std::string& s, size_t pos) {
		size_t end = pos;
		while (end < s.size() && IsIdentChar(s[end]))
			end++;
		return std::string_view(s).substr(pos, end - pos);
	}

	std::string Trim(std::string_view v) {
		while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.remove_prefix(1);
		while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.remove_suffix(1);
		return std::string(v);
	}

	// comments -> spaces (newlines inside block comments are kept)
	std::string StripComments(const std::string& s) {
		std::string out;
		out.reserve(s.size());
		for (size_t i = 0; i < s.size(); ) {
			if (s.compare(i, 2, "//") == 0) {
				while (i < s.size() && s[i] != '\n')
					i++;
				out += ' ';
			} else if (s.compare(i, 2, "/*") == 0) {
				const size_t end = s.find("*/", i + 2);
				const size_t stop = (end == std::string::npos)? s.size(): end + 2;
				for (; i < stop; i++)
					out += (s[i] == '\n')? '\n': ' ';
			} else {
				out += s[i++];
			}
		}
		return out;
	}

	// blank out a line (keeps the newline, so line numbers in compile logs stay right)
	void BlankLineAt(std::string& s, size_t pos) {
		size_t begin = s.rfind('\n', pos);
		begin = (begin == std::string::npos)? 0: begin + 1;
		size_t end = s.find('\n', pos);
		if (end == std::string::npos)
			end = s.size();
		s.replace(begin, end - begin, "//");
	}

	// `#version N [profile]`: returns N (110 when absent) and removes the directive
	int ExtractVersion(std::string& s) {
		int version = 110;
		for (size_t pos = s.find("#version"); pos != std::string::npos; pos = s.find("#version", pos)) {
			// ignore the text when it sits in a // comment
			const size_t lineBegin = s.rfind('\n', pos);
			const std::string_view before = std::string_view(s).substr((lineBegin == std::string::npos)? 0: lineBegin + 1, pos - ((lineBegin == std::string::npos)? 0: lineBegin + 1));
			if (before.find("//") != std::string_view::npos) {
				pos++;
				continue;
			}
			version = std::atoi(s.c_str() + pos + 8);
			BlankLineAt(s, pos);
		}
		return version;
	}

	// desktop extension directives mean nothing to the GLSL ES compiler, and it rejects
	// any #extension after the first declaration (our preamble)
	void StripExtensions(std::string& s) {
		for (size_t pos = s.find("#extension"); pos != std::string::npos; pos = s.find("#extension", pos))
			BlankLineAt(s, pos);
	}

	// Desktop compilers predefine a macro per supported extension (GL_ARB_foo == 1) and the
	// engine's shaders test them in #if lines. GLSL ES has none of them, treats an undefined
	// identifier in #if as an error, and reserves the GL_ prefix so they cannot be #defined.
	// In preprocessor lines: `defined(GL_ARB_foo)` / #ifdef -> an undefined name, else -> 0.
	void NeutralizeExtensionMacros(std::string& s) {
		for (size_t pos = s.find("GL_"); pos != std::string::npos; pos = s.find("GL_", pos + 1)) {
			if (pos > 0 && IsIdentChar(s[pos - 1]))
				continue;
			const std::string_view id = ReadIdent(s, pos);
			bool isExt = false;
			for (const std::string_view vendor: {"GL_ARB_", "GL_EXT_", "GL_NV_", "GL_AMD_", "GL_ATI_", "GL_OES_", "GL_KHR_", "GL_MESA_", "GL_INTEL_", "GL_APPLE_"})
				isExt |= (id.substr(0, vendor.size()) == vendor);
			if (!isExt)
				continue;

			size_t lineBegin = s.rfind('\n', pos);
			lineBegin = SkipSpace(s, (lineBegin == std::string::npos)? 0: lineBegin + 1);
			if (s[lineBegin] != '#')
				continue;

			const std::string_view before = std::string_view(s).substr(lineBegin, pos - lineBegin);
			size_t tail = before.find_last_not_of(" \t(");
			const std::string_view lastWord = (tail == std::string_view::npos)? std::string_view(): before.substr(0, tail + 1);
			const bool existenceTest =
				(lastWord.size() >= 7 && lastWord.substr(lastWord.size() - 7) == "defined") ||
				(lastWord.size() >= 5 && lastWord.substr(lastWord.size() - 5) == "ifdef") ||
				(lastWord.size() >= 6 && lastWord.substr(lastWord.size() - 6) == "ifndef");
			s.replace(pos, id.size(), existenceTest? "RCL_NO_SUCH_EXTENSION": "0");
		}
	}

	// `inst.member` -> `repl` for every member access through the block instance name
	void ReplaceMemberAccess(std::string& s, std::string_view inst, std::string_view member, std::string_view repl) {
		for (size_t pos = FindIdent(s, inst); pos != std::string::npos; pos = FindIdent(s, inst, pos + 1)) {
			size_t p = SkipSpace(s, pos + inst.size());
			if (p >= s.size() || s[p] != '.')
				continue;
			p = SkipSpace(s, p + 1);
			if (ReadIdent(s, p) != member)
				continue;
			s.replace(pos, p + member.size() - pos, repl);
		}
	}

	// GLSL ES 3.00 has no in/out interface blocks:
	//   out Data { vec4 col; vec2 uv; };        -> out vec4 col; out vec2 uv;
	//   out Data { vec4 col; } vsOut;           -> out vec4 Data_col;   (vsOut.col -> Data_col)
	// Both stages flatten the same block to the same names, so they still link.
	void FlattenInterfaceBlocks(std::string& s) {
		for (const std::string_view qual: {std::string_view("in"), std::string_view("out")}) {
			size_t pos = 0;
			while ((pos = FindIdent(s, qual, pos)) != std::string::npos) {
				const size_t namePos = SkipSpace(s, pos + qual.size());
				const std::string blockName(ReadIdent(s, namePos));
				const size_t bracePos = SkipSpace(s, namePos + blockName.size());

				if (blockName.empty() || bracePos >= s.size() || s[bracePos] != '{') {
					pos += qual.size();
					continue;
				}
				const size_t closePos = s.find('}', bracePos);
				if (closePos == std::string::npos)
					break;

				size_t endPos = SkipSpace(s, closePos + 1);
				const std::string instName(ReadIdent(s, endPos));
				endPos = SkipSpace(s, endPos + instName.size());
				if (endPos >= s.size() || s[endPos] != ';') {
					// arrays of blocks and anything else unexpected: leave it to the compiler
					pos = closePos;
					continue;
				}

				// members: "[qualifiers] type name[N]" separated by ';'. Comments are dropped,
				// preprocessor lines (#if around members) are kept on lines of their own, and
				// so are the newlines, which keeps the line numbering of the rest.
				std::string decls;
				std::vector<std::pair<std::string, std::string>> renames; // member -> flattened name
				const std::string body = StripComments(s.substr(bracePos + 1, closePos - bracePos - 1));

				const auto addMember = [&](std::string member) {
					member = Trim(member);
					if (member.empty())
						return;

					if (!instName.empty()) {
						// the member name is the last identifier before an optional array suffix
						size_t nameEnd = member.find('[');
						if (nameEnd == std::string::npos)
							nameEnd = member.size();
						while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(member[nameEnd - 1])))
							nameEnd--;
						size_t nameBegin = nameEnd;
						while (nameBegin > 0 && IsIdentChar(member[nameBegin - 1]))
							nameBegin--;
						const std::string memberName = member.substr(nameBegin, nameEnd - nameBegin);
						const std::string flatName = blockName + "_" + memberName;
						member.replace(nameBegin, nameEnd - nameBegin, flatName);
						renames.emplace_back(memberName, flatName);
					}
					decls += std::string(qual) + " " + member + "; ";
				};

				std::string member;
				for (size_t lineBegin = 0; lineBegin <= body.size(); ) {
					size_t lineEnd = body.find('\n', lineBegin);
					if (lineEnd == std::string::npos)
						lineEnd = body.size();
					const std::string_view line = std::string_view(body).substr(lineBegin, lineEnd - lineBegin);
					const size_t first = line.find_first_not_of(" \t\r");

					if (first != std::string_view::npos && line[first] == '#') {
						// preprocessor directives must start a line
						if (!decls.empty() && decls.back() != '\n')
							decls += '\n';
						decls += line;
					} else {
						for (const char c: line) {
							if (c == ';') {
								addMember(member);
								member.clear();
							} else {
								member += c;
							}
						}
						member += ' ';
					}
					if (lineEnd < body.size())
						decls += '\n';
					lineBegin = lineEnd + 1;
				}
				addMember(member);

				s.replace(pos, endPos + 1 - pos, decls);
				for (const auto& [memberName, flatName]: renames)
					ReplaceMemberAccess(s, instName, memberName, flatName);
				pos += decls.size();
			}
		}
	}

	// `uniform T name = expr;` -> `uniform T name;`, recording the initializer's literals
	void StripUniformInitializers(std::string& s, std::vector<WebGLCompat::UniformDefault>& defaults) {
		for (size_t pos = FindIdent(s, "uniform"); pos != std::string::npos; pos = FindIdent(s, "uniform", pos + 1)) {
			const size_t end = s.find_first_of(";{", pos);
			if (end == std::string::npos || s[end] == '{')
				continue;
			const size_t eq = s.find('=', pos);
			if (eq == std::string::npos || eq > end)
				continue;

			// name: last identifier before '=' (skipping an array suffix); type: the one before it
			size_t nameEnd = eq;
			while (nameEnd > pos && (std::isspace(static_cast<unsigned char>(s[nameEnd - 1])) || s[nameEnd - 1] == ']' || s[nameEnd - 1] == '[' || std::isdigit(static_cast<unsigned char>(s[nameEnd - 1]))))
				nameEnd--;
			size_t nameBegin = nameEnd;
			while (nameBegin > pos && IsIdentChar(s[nameBegin - 1]))
				nameBegin--;
			size_t typeEnd = nameBegin;
			while (typeEnd > pos && std::isspace(static_cast<unsigned char>(s[typeEnd - 1])))
				typeEnd--;
			size_t typeBegin = typeEnd;
			while (typeBegin > pos && IsIdentChar(s[typeBegin - 1]))
				typeBegin--;

			WebGLCompat::UniformDefault def;
			def.name = s.substr(nameBegin, nameEnd - nameBegin);
			def.type = s.substr(typeBegin, typeEnd - typeBegin);

			for (size_t p = eq + 1; p < end; ) {
				const char c = s[p];
				if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
					const std::string_view id = ReadIdent(s, p);
					if (id == "true") def.values.push_back(1.0f);
					if (id == "false") def.values.push_back(0.0f);
					p += id.size();
				} else if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || ((c == '-' || c == '+') && p + 1 < end && (std::isdigit(static_cast<unsigned char>(s[p + 1])) || s[p + 1] == '.'))) {
					char* numEnd = nullptr;
					def.values.push_back(std::strtof(s.c_str() + p, &numEnd));
					p = std::max<size_t>(p + 1, numEnd - s.c_str());
					// literal suffixes (1.0f, 2u)
					while (p < end && std::isalpha(static_cast<unsigned char>(s[p])))
						p++;
				} else {
					p++;
				}
			}
			defaults.push_back(std::move(def));

			// keep any newlines of the removed text so line numbers stay right
			const size_t newlines = std::count(s.begin() + eq, s.begin() + end, '\n');
			s.replace(eq, end - eq, std::string(newlines, '\n'));
		}
	}

	// GLSL ES needs an explicit location on every fragment output once there is more than
	// one (desktop assigns them at link time, optionally steered by glBindFragDataLocation).
	// Finds the global `out T name;` declarations without a layout qualifier; with more than
	// one, or when `assign` is set, gives each a location: the bound one, else the next free.
	// Returns the number of such declarations.
	int AssignFragOutputLocations(std::string& s, const std::vector<std::pair<std::string, int>>* bound, bool assign) {
		struct Decl { size_t pos; std::string name; };
		std::vector<Decl> decls;

		for (size_t pos = FindIdent(s, "out"); pos != std::string::npos; pos = FindIdent(s, "out", pos + 1)) {
			// a global declaration starts a statement: what precedes it is the end of the
			// previous one (';' or '}') or a preprocessor line. That rules out function
			// parameters ('(' or ',' before) and outputs with a layout(...) (')' before).
			size_t before = pos;
			while (before > 0 && std::isspace(static_cast<unsigned char>(s[before - 1])))
				before--;
			if (before > 0 && s[before - 1] != ';' && s[before - 1] != '}') {
				size_t lineBegin = s.rfind('\n', before - 1);
				lineBegin = SkipSpace(s, (lineBegin == std::string::npos)? 0: lineBegin + 1);
				if (s[lineBegin] != '#' && s.compare(lineBegin, 2, "//") != 0)
					continue;
			}

			const size_t end = s.find(';', pos);
			if (end == std::string::npos)
				break;
			size_t nameEnd = s.find('[', pos);
			if (nameEnd == std::string::npos || nameEnd > end)
				nameEnd = end;
			while (nameEnd > pos && std::isspace(static_cast<unsigned char>(s[nameEnd - 1])))
				nameEnd--;
			size_t nameBegin = nameEnd;
			while (nameBegin > pos && IsIdentChar(s[nameBegin - 1]))
				nameBegin--;
			decls.push_back({pos, s.substr(nameBegin, nameEnd - nameBegin)});
			pos = end;
		}

		if (decls.size() > 1 || (assign && !decls.empty())) {
			uint32_t usedLocations = 0;
			std::vector<int> locations(decls.size(), -1);
			for (size_t i = 0; i < decls.size(); i++) {
				if (bound == nullptr)
					break;
				for (const auto& [name, index]: *bound) {
					if (name == decls[i].name && index >= 0 && index < 32) {
						locations[i] = index;
						usedLocations |= (1u << index);
					}
				}
			}
			for (size_t i = 0; i < decls.size(); i++) {
				if (locations[i] >= 0)
					continue;
				int loc = 0;
				while ((usedLocations & (1u << loc)) != 0)
					loc++;
				locations[i] = loc;
				usedLocations |= (1u << loc);
			}
			// back to front, so the recorded positions stay valid
			for (size_t i = decls.size(); i-- > 0; )
				s.insert(decls[i].pos, "layout(location = " + std::to_string(locations[i]) + ") ");
		}
		return int(decls.size());
	}
}


namespace {
	bool clipDistanceSupport = false;
}

void WebGLCompat::SetClipDistanceSupport(bool supported) { clipDistanceSupport = supported; }
bool WebGLCompat::HaveClipDistanceSupport() { return clipDistanceSupport; }

WebGLCompat::ShaderTranslation WebGLCompat::TranslateShader(bool isVertex, const std::string& src, uint32_t forceVaryings, const std::vector<std::pair<std::string, int>>* fragOutputBindings, bool forGlslang)
{
	ShaderTranslation res;
	std::string s = src;

	res.glslVersion = ExtractVersion(s);
	StripExtensions(s);
	NeutralizeExtensionMacros(s);
	if (forGlslang) {
		// resolve the preprocessor first (the original #version line is blanked, so
		// "#line 1" keeps the numbering); on failure the rewrites see the raw text
		std::string pp;
		const std::string versioned = "#version " + std::to_string(res.glslVersion) + ((res.glslVersion >= 150)? " compatibility": "") + "\n#line 1\n" + s;
		if (PreprocessGlsl(versioned, pp)) {
			s = std::move(pp);
			ExtractVersion(s);
			StripExtensions(s);
		}
	}
	FlattenInterfaceBlocks(s);
	StripUniformInitializers(s, res.uniformDefaults);

	std::string pre;
	pre.reserve(2048);
	if (forGlslang) {
		// SPIR-V for OpenGL needs GLSL 3.30 or later; the rewrites below already turned the
		// compatibility-profile parts into core GLSL. layout(binding) and line continuations
		// need 4.20 (GL_ARB_shading_language_420pack, stripped with the other extensions).
		int version = std::max(res.glslVersion, 330);
		if (version < 420 && (HasIdent(s, "binding") || s.find("\\\n") != std::string::npos || s.find("\\\r\n") != std::string::npos))
			version = 420;
		pre += "#version " + std::to_string(version) + " core\n";
	} else {
		pre += "#version 300 es\n";
		if (isVertex && clipDistanceSupport && (HasIdent(s, "gl_ClipVertex") || HasIdent(s, "gl_ClipDistance")))
			pre += "#extension GL_ANGLE_clip_cull_distance : require\n";
		pre += "precision highp float;\nprecision highp int;\n";
		for (const char* samplerType: {
			"sampler2D", "sampler3D", "samplerCube", "sampler2DArray",
			"sampler2DShadow", "samplerCubeShadow", "sampler2DArrayShadow",
			"isampler2D", "isampler3D", "isamplerCube", "isampler2DArray",
			"usampler2D", "usampler3D", "usamplerCube", "usampler2DArray",
		}) {
			pre += "precision highp ";
			pre += samplerType;
			pre += ";\n";
		}
	}

	// --- pre-1.30 storage qualifiers and texture functions
	if (isVertex) {
		ReplaceIdent(s, "attribute", "in");
		ReplaceIdent(s, "varying", "out");
	} else {
		ReplaceIdent(s, "varying", "in");
	}
	// not in GLSL ES; costs perspective-correct instead of linear interpolation
	ReplaceIdent(s, "noperspective", "");

	static constexpr std::pair<std::string_view, std::string_view> texFuncs[] = {
		{"texture1D", "texture"}, {"texture2D", "texture"}, {"texture3D", "texture"}, {"textureCube", "texture"},
		{"texture2DArray", "texture"},
		{"texture2DLod", "textureLod"}, {"texture3DLod", "textureLod"}, {"textureCubeLod", "textureLod"},
		{"texture2DProj", "textureProj"}, {"texture3DProj", "textureProj"},
		{"texture2DProjLod", "textureProjLod"},
		{"texture2DGrad", "textureGrad"}, {"textureCubeGrad", "textureGrad"},
		{"shadow2D", "rcl_shadow2D"}, {"shadow2DProj", "rcl_shadow2DProj"},
	};
	bool usesShadowFuncs = false;
	for (const auto& [from, to]: texFuncs) {
		const int n = ReplaceIdent(s, from, to);
		usesShadowFuncs |= (n > 0 && to.substr(0, 4) == "rcl_");
	}
	if (usesShadowFuncs) {
		// the pre-1.30 shadow lookups return vec4
		pre += "vec4 rcl_shadow2D(sampler2DShadow s, vec3 c) { return vec4(texture(s, c)); }\n";
		pre += "vec4 rcl_shadow2DProj(sampler2DShadow s, vec4 c) { return vec4(textureProj(s, c)); }\n";
	}

	// --- compatibility-profile uniforms (fixed-function state, uploaded by WebGLCompat)
	static constexpr std::pair<std::string_view, std::string_view> matrices[] = {
		{"gl_ModelViewProjectionMatrixInverse", "uniform mat4 rcl_ModelViewProjectionMatrixInverse;\n"},
		{"gl_ModelViewProjectionMatrix", "uniform mat4 rcl_ModelViewProjectionMatrix;\n"},
		{"gl_ModelViewMatrixInverse", "uniform mat4 rcl_ModelViewMatrixInverse;\n"},
		{"gl_ModelViewMatrix", "uniform mat4 rcl_ModelViewMatrix;\n"},
		{"gl_ProjectionMatrixInverse", "uniform mat4 rcl_ProjectionMatrixInverse;\n"},
		{"gl_ProjectionMatrix", "uniform mat4 rcl_ProjectionMatrix;\n"},
		{"gl_NormalMatrix", "uniform mat3 rcl_NormalMatrix;\n"},
		{"gl_TextureMatrix", "uniform mat4 rcl_TextureMatrix[4];\n"},
	};
	for (const auto& [name, decl]: matrices) {
		if (ReplaceIdent(s, name, std::string("rcl_") + std::string(name.substr(3))) > 0)
			pre += decl;
	}
	if (ReplaceIdent(s, "ftransform", "rcl_ftransform") > 0) {
		// rcl_ftransform() itself is defined after rcl_Vertex below
		if (!HasIdent(pre, "rcl_ModelViewProjectionMatrix"))
			pre += "uniform mat4 rcl_ModelViewProjectionMatrix;\n";
	}
	if (ReplaceIdent(s, "gl_Fog", "rcl_Fog") > 0) {
		pre += "struct rcl_FogParameters { vec4 color; float density; float start; float end; float scale; };\n";
		pre += "uniform rcl_FogParameters rcl_Fog;\n";
	}
	if (ReplaceIdent(s, "gl_LightSource", "rcl_LightSource") > 0) {
		pre += "struct rcl_LightSourceParameters { vec4 ambient; vec4 diffuse; vec4 specular; vec4 position; vec4 halfVector; vec3 spotDirection; float spotExponent; float spotCutoff; float spotCosCutoff; float constantAttenuation; float linearAttenuation; float quadraticAttenuation; };\n";
		pre += "uniform rcl_LightSourceParameters rcl_LightSource[2];\n";
	}

	// --- compatibility varyings
	uint32_t varyings = forceVaryings;
	if (isVertex) {
		if (ReplaceIdent(s, "gl_FrontColor", "rcl_VaryingColor") > 0) varyings |= VARYING_COLOR;
		if (ReplaceIdent(s, "gl_TexCoord", "rcl_VaryingTexCoord") > 0) varyings |= VARYING_TEXCOORD;
		if (ReplaceIdent(s, "gl_FogFragCoord", "rcl_VaryingFogCoord") > 0) varyings |= VARYING_FOGCOORD;

		if (varyings & VARYING_COLOR) pre += "out vec4 rcl_VaryingColor;\n";
		if (varyings & VARYING_TEXCOORD) pre += "out vec4 rcl_VaryingTexCoord[8];\n";
		if (varyings & VARYING_FOGCOORD) pre += "out float rcl_VaryingFogCoord;\n";

		// GLSL 1.50 redeclarations (`out float gl_ClipDistance[2];`) would clash with the sink below
		for (size_t pos = s.find("out float gl_ClipDistance"); pos != std::string::npos; pos = s.find("out float gl_ClipDistance", pos))
			BlankLineAt(s, pos);

		// outputs that go nowhere in WebGL2 (two-sided/secondary color, user clip planes)
		static constexpr std::pair<std::string_view, std::string_view> sinks[] = {
			{"gl_BackColor", "vec4 rcl_BackColor;\n"},
			{"gl_FrontSecondaryColor", "vec4 rcl_FrontSecondaryColor;\n"},
			{"gl_BackSecondaryColor", "vec4 rcl_BackSecondaryColor;\n"},
			{"gl_ClipVertex", "vec4 rcl_ClipVertex;\n"},
			{"gl_ClipDistance", "float rcl_ClipDistance[8];\n"},
		};
		bool clipVertex = false;
		if (clipDistanceSupport) {
			// real clipping: gl_ClipDistance stays, gl_ClipVertex is turned into distances
			// against the planes the shim tracks (in eye space, like desktop glClipPlane)
			res.writesClipDistance = HasIdent(s, "gl_ClipDistance");
			clipVertex = (ReplaceIdent(s, "gl_ClipVertex", "rcl_ClipVertex") > 0);
			if (clipVertex) {
				pre += "vec4 rcl_ClipVertex = vec4(0.0, 0.0, 0.0, 1.0);\n";
				pre += "uniform vec4 rcl_ClipPlane[" + std::to_string(NUM_CLIP_PLANES) + "];\n";
				ReplaceIdent(s, "main", "rcl_main");
				res.writesClipDistance = true;
			}
		}
		for (const auto& [name, decl]: sinks) {
			if (ReplaceIdent(s, name, std::string("rcl_") + std::string(name.substr(3))) > 0)
				pre += decl;
		}
		if (clipVertex) {
			// after the shader's own main (which may return early)
			s += "\nvoid main() {\n\trcl_main();\n";
			for (int i = 0; i < NUM_CLIP_PLANES; i++)
				s += "\tgl_ClipDistance[" + std::to_string(i) + "] = dot(rcl_ClipVertex, rcl_ClipPlane[" + std::to_string(i) + "]);\n";
			s += "}\n";
		}

		// --- fixed-function vertex attributes (bound to their conventional locations at link time)
		// gl_Color in a vertex shader is the color attribute
		static constexpr std::pair<std::string_view, uint32_t> attribs[] = {
			{"gl_Vertex", ATTRIB_VERTEX}, {"gl_Normal", ATTRIB_NORMAL}, {"gl_Color", ATTRIB_COLOR},
			{"gl_MultiTexCoord0", ATTRIB_TEXCOORD0 + 0}, {"gl_MultiTexCoord1", ATTRIB_TEXCOORD0 + 1},
			{"gl_MultiTexCoord2", ATTRIB_TEXCOORD0 + 2}, {"gl_MultiTexCoord3", ATTRIB_TEXCOORD0 + 3},
			{"gl_MultiTexCoord4", ATTRIB_TEXCOORD0 + 4}, {"gl_MultiTexCoord5", ATTRIB_TEXCOORD0 + 5},
			{"gl_MultiTexCoord6", ATTRIB_TEXCOORD0 + 6}, {"gl_MultiTexCoord7", ATTRIB_TEXCOORD0 + 7},
		};
		const bool usesFtransform = HasIdent(s, "rcl_ftransform");
		for (const auto& [name, loc]: attribs) {
			const std::string newName = std::string("rcl_") + std::string(name.substr(3));
			if (ReplaceIdent(s, name, newName) == 0 && !(usesFtransform && loc == ATTRIB_VERTEX))
				continue;
			pre += (loc == ATTRIB_NORMAL)? "in vec3 ": "in vec4 ";
			pre += newName;
			pre += ";\n";
			res.attribsUsed |= (1u << loc);
		}
		if (usesFtransform)
			pre += "vec4 rcl_ftransform() { return rcl_ModelViewProjectionMatrix * rcl_Vertex; }\n";
	} else {
		if (ReplaceIdent(s, "gl_Color", "rcl_VaryingColor") > 0) varyings |= VARYING_COLOR;
		if (ReplaceIdent(s, "gl_TexCoord", "rcl_VaryingTexCoord") > 0) varyings |= VARYING_TEXCOORD;
		if (ReplaceIdent(s, "gl_FogFragCoord", "rcl_VaryingFogCoord") > 0) varyings |= VARYING_FOGCOORD;

		if (varyings & VARYING_COLOR) pre += "in vec4 rcl_VaryingColor;\n";
		if (varyings & VARYING_TEXCOORD) pre += "in vec4 rcl_VaryingTexCoord[8];\n";
		if (varyings & VARYING_FOGCOORD) pre += "in float rcl_VaryingFogCoord;\n";

		// --- fragment outputs
		res.numFragOutputs = AssignFragOutputLocations(s, fragOutputBindings, fragOutputBindings != nullptr);

		// gl_FragData becomes an output array, so indices may be macros or expressions and
		// outputs can be read back; a shader using both (in different #if branches) gets
		// gl_FragColor as element 0
		if (ReplaceIdent(s, "gl_FragData", "rcl_FragData") > 0) {
			pre += "layout(location = 0) out vec4 rcl_FragData[8];\n";
			ReplaceIdent(s, "gl_FragColor", "rcl_FragData[0]");
		} else if (ReplaceIdent(s, "gl_FragColor", "rcl_FragColor") > 0) {
			pre += "layout(location = 0) out vec4 rcl_FragColor;\n";
		}
	}
	res.varyingsUsed = varyings;

	// keep the engine's own #line numbering for whatever follows
	pre += "#line 1\n";
	res.source = pre + s;
	return res;
}
