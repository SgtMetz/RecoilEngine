/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Source-level translation of the engine's desktop GLSL (1.10 - 3.30, compatibility
// profile) to GLSL ES 3.00 for WebGL2. Text based, no type analysis: it rewrites the
// version, storage qualifiers, built-in names and interface blocks, and declares the
// compatibility-profile built-ins as ordinary attributes/uniforms/varyings (rcl_*) that
// WebGLCompat feeds from its fixed-function state emulation. What it cannot fix (e.g.
// implicit int -> float conversions, geometry shaders, GLSL 4 features) is left to fail
// in the WebGL compiler, whose log reaches the engine as usual.
namespace WebGLCompat {
	// compatibility varyings shared between the vertex and fragment stage
	enum CompatVarying: uint32_t {
		VARYING_COLOR    = 1 << 0, // gl_FrontColor -> gl_Color
		VARYING_TEXCOORD = 1 << 1, // gl_TexCoord[]
		VARYING_FOGCOORD = 1 << 2, // gl_FogFragCoord
	};

	// conventional generic attribute locations of the fixed-function vertex attributes
	// (the aliasing desktop compatibility contexts use; the client-array emulation and
	// glBindAttribLocation at link time both follow it)
	enum CompatAttrib: uint32_t {
		ATTRIB_VERTEX    = 0,
		ATTRIB_NORMAL    = 2,
		ATTRIB_COLOR     = 3,
		ATTRIB_TEXCOORD0 = 8, // .. 15
	};

	// `uniform float x = 0.5;` is not allowed in GLSL ES: the initializer is removed
	// and applied with glUniform* after linking
	struct UniformDefault {
		std::string name;
		std::string type;          // float, int, bool, vec2..vec4
		std::vector<float> values; // the numeric literals of the initializer, in order
	};

	struct ShaderTranslation {
		std::vector<UniformDefault> uniformDefaults;
		std::string source;         // GLSL ES 3.00
		uint32_t varyingsUsed = 0;  // CompatVarying mask the stage reads or writes
		uint32_t attribsUsed = 0;   // bit n set: generic attribute n is a fixed-function built-in
		int glslVersion = 110;      // version the original source asked for
		int numFragOutputs = 0;     // (fragment) user-declared outputs without a layout qualifier
	};

	// isVertex: vertex stage, otherwise fragment
	// forceVaryings: (vertex stage) also declare these CompatVaryings, because the
	//   fragment stage reads them and GLSL ES requires a matching vertex output
	// fragOutputBindings: (fragment stage) glBindFragDataLocation name -> index pairs; when
	//   given, every user-declared output without a layout qualifier gets a location
	ShaderTranslation TranslateShader(bool isVertex, const std::string& src, uint32_t forceVaryings = 0, const std::vector<std::pair<std::string, int>>* fragOutputBindings = nullptr);
}
