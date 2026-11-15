/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Fixed-function state emulation for the WebGL2 shim (see WebGLCompat.h):
//  - matrix stacks (glMatrixMode & co), fog and light parameters, kept on the CPU and
//    uploaded as rcl_* uniforms to programs whose shaders use the gl_* built-ins
//  - shader source translation at glShaderSource, attribute binding at glLinkProgram
//  - client-state vertex arrays mapped onto generic attributes
//  - desktop-only enable caps, primitive modes and glPushAttrib/glPopAttrib

#include "WebGLInternal.h"
#include "WebGLShaderTranslate.h"

#include <glad/glad.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "System/Log/ILog.h"

namespace {
	// ---- helpers -----------------------------------------------------------------
	#define REAL(name) decltype(glad_##name) real_##name = nullptr
	#define HOOK(name) do { real_##name = glad_##name; glad_##name = compat_##name; } while (0)

	struct Mat4 {
		float m[16]; // column-major, like GL

		static Mat4 Identity() { return {{1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1}}; }
	};

	// a * b
	Mat4 Mul(const Mat4& a, const Mat4& b) {
		Mat4 r;
		for (int c = 0; c < 4; c++) {
			for (int row = 0; row < 4; row++) {
				r.m[c * 4 + row] =
					a.m[0 * 4 + row] * b.m[c * 4 + 0] +
					a.m[1 * 4 + row] * b.m[c * 4 + 1] +
					a.m[2 * 4 + row] * b.m[c * 4 + 2] +
					a.m[3 * 4 + row] * b.m[c * 4 + 3];
			}
		}
		return r;
	}

	// general inverse (cofactors); returns identity for singular matrices
	Mat4 Inverse(const Mat4& in) {
		const float* m = in.m;
		float inv[16];

		inv[ 0] =  m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
		inv[ 4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
		inv[ 8] =  m[4] * m[ 9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[ 9];
		inv[12] = -m[4] * m[ 9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[ 9];
		inv[ 1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
		inv[ 5] =  m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
		inv[ 9] = -m[0] * m[ 9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[ 9];
		inv[13] =  m[0] * m[ 9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[ 9];
		inv[ 2] =  m[1] * m[ 6] * m[15] - m[1] * m[ 7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[ 7] - m[13] * m[3] * m[ 6];
		inv[ 6] = -m[0] * m[ 6] * m[15] + m[0] * m[ 7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[ 7] + m[12] * m[3] * m[ 6];
		inv[10] =  m[0] * m[ 5] * m[15] - m[0] * m[ 7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[ 7] - m[12] * m[3] * m[ 5];
		inv[14] = -m[0] * m[ 5] * m[14] + m[0] * m[ 6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[ 6] + m[12] * m[2] * m[ 5];
		inv[ 3] = -m[1] * m[ 6] * m[11] + m[1] * m[ 7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[ 9] * m[2] * m[ 7] + m[ 9] * m[3] * m[ 6];
		inv[ 7] =  m[0] * m[ 6] * m[11] - m[0] * m[ 7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[ 8] * m[2] * m[ 7] - m[ 8] * m[3] * m[ 6];
		inv[11] = -m[0] * m[ 5] * m[11] + m[0] * m[ 7] * m[ 9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[ 9] - m[ 8] * m[1] * m[ 7] + m[ 8] * m[3] * m[ 5];
		inv[15] =  m[0] * m[ 5] * m[10] - m[0] * m[ 6] * m[ 9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[ 9] + m[ 8] * m[1] * m[ 6] - m[ 8] * m[2] * m[ 5];

		const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
		if (det == 0.0f)
			return Mat4::Identity();

		Mat4 r;
		for (int i = 0; i < 16; i++)
			r.m[i] = inv[i] / det;
		return r;
	}

	// ---- fixed-function state -------------------------------------------------------
	constexpr int MAX_TEX_UNITS = 8;

	struct MatrixStack {
		std::vector<Mat4> stack{Mat4::Identity()};
		Mat4& Top() { return stack.back(); }
	};

	MatrixStack modelviewStack;
	MatrixStack projectionStack;
	MatrixStack textureStacks[MAX_TEX_UNITS];
	GLenum matrixMode = GL_MODELVIEW;
	int activeTexUnit = 0;
	int clientActiveTexUnit = 0;

	// bumped whenever any state uploaded as rcl_* uniforms changes
	uint32_t fixedStateStamp = 1;

	struct FogState { float color[4] = {0, 0, 0, 0}; float density = 1, start = 0, end = 1; } fog;
	struct LightState { float ambient[4] = {0, 0, 0, 1}, diffuse[4] = {1, 1, 1, 1}, specular[4] = {1, 1, 1, 1}, position[4] = {0, 0, 1, 0}; } lights[2];
	float currentColor[4] = {1, 1, 1, 1};

	MatrixStack& CurStack() {
		switch (matrixMode) {
			case GL_PROJECTION: return projectionStack;
			case GL_TEXTURE: return textureStacks[std::min(activeTexUnit, MAX_TEX_UNITS - 1)];
			default: return modelviewStack;
		}
	}

	// display lists record state changes as closures (see WebGLImmediate.cpp); a function
	// that starts with LIST(itself) is skipped and recorded while a list is being compiled
	#define LIST(...) do { if (WebGLCompat::listCompiling) { WebGLCompat::ListRecord([=] { __VA_ARGS__; }); return; } } while (0)

	// a non-finite matrix silently clips every later draw: log the first few operations
	// that produce one
	int nonFiniteReports = 0;
	void CheckFinite(const char* op, const Mat4& prev, const Mat4& arg) {
		const Mat4& top = CurStack().Top();
		bool finite = true;
		for (float v: top.m)
			finite = finite && std::isfinite(v);
		if (finite || nonFiniteReports >= 5)
			return;
		nonFiniteReports++;
		LOG_L(L_WARNING, "[WebGLCompat] %s made the 0x%x matrix non-finite (in list: %d); arg diag=(%g %g %g %g) trans=(%g %g %g); previous diag=(%g %g %g %g)",
			op, matrixMode, int(WebGLCompat::GetCompilingList() != 0), arg.m[0], arg.m[5], arg.m[10], arg.m[15], arg.m[12], arg.m[13], arg.m[14],
			prev.m[0], prev.m[5], prev.m[10], prev.m[15]);
	}
	void MultCur(Mat4 m) { LIST(MultCur(m)); const Mat4 prev = CurStack().Top(); CurStack().Top() = Mul(prev, m); fixedStateStamp++; CheckFinite("glMult/Translate/Scale/Rotate/Ortho", prev, m); }
	void LoadCur(Mat4 m) { LIST(LoadCur(m)); const Mat4 prev = CurStack().Top(); CurStack().Top() = m; fixedStateStamp++; CheckFinite("glLoadMatrix", prev, m); }

	void APIENTRY compat_glMatrixMode(GLenum mode) { LIST(compat_glMatrixMode(mode)); matrixMode = mode; }
	void APIENTRY compat_glLoadIdentity() { LoadCur(Mat4::Identity()); }
	void APIENTRY compat_glLoadMatrixf(const GLfloat* m) { Mat4 t; std::memcpy(t.m, m, sizeof(Mat4)); LoadCur(t); }
	void APIENTRY compat_glLoadMatrixd(const GLdouble* m) { Mat4 t; for (int i = 0; i < 16; i++) t.m[i] = float(m[i]); LoadCur(t); }
	void APIENTRY compat_glMultMatrixf(const GLfloat* m) { Mat4 t; std::memcpy(t.m, m, sizeof(Mat4)); MultCur(t); }
	void APIENTRY compat_glMultMatrixd(const GLdouble* m) { Mat4 t; for (int i = 0; i < 16; i++) t.m[i] = float(m[i]); MultCur(t); }
	void APIENTRY compat_glPushMatrix() { LIST(compat_glPushMatrix()); MatrixStack& s = CurStack(); s.stack.push_back(s.stack.back()); }
	void APIENTRY compat_glPopMatrix() { LIST(compat_glPopMatrix()); MatrixStack& s = CurStack(); if (s.stack.size() > 1) { s.stack.pop_back(); fixedStateStamp++; } }

	void APIENTRY compat_glTranslatef(GLfloat x, GLfloat y, GLfloat z) { Mat4 t = Mat4::Identity(); t.m[12] = x; t.m[13] = y; t.m[14] = z; MultCur(t); }
	void APIENTRY compat_glTranslated(GLdouble x, GLdouble y, GLdouble z) { compat_glTranslatef(float(x), float(y), float(z)); }
	void APIENTRY compat_glScalef(GLfloat x, GLfloat y, GLfloat z) { Mat4 t = Mat4::Identity(); t.m[0] = x; t.m[5] = y; t.m[10] = z; MultCur(t); }
	void APIENTRY compat_glScaled(GLdouble x, GLdouble y, GLdouble z) { compat_glScalef(float(x), float(y), float(z)); }

	void APIENTRY compat_glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
		const float len = std::sqrt(x * x + y * y + z * z);
		if (len == 0.0f)
			return;
		x /= len; y /= len; z /= len;
		const float rad = angle * 3.14159265358979323846f / 180.0f;
		const float c = std::cos(rad), s = std::sin(rad), ic = 1.0f - c;
		Mat4 t = Mat4::Identity();
		t.m[0] = x * x * ic + c;     t.m[4] = x * y * ic - z * s; t.m[ 8] = x * z * ic + y * s;
		t.m[1] = y * x * ic + z * s; t.m[5] = y * y * ic + c;     t.m[ 9] = y * z * ic - x * s;
		t.m[2] = z * x * ic - y * s; t.m[6] = z * y * ic + x * s; t.m[10] = z * z * ic + c;
		MultCur(t);
	}
	void APIENTRY compat_glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z) { compat_glRotatef(float(angle), float(x), float(y), float(z)); }

	void APIENTRY compat_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
		Mat4 m = Mat4::Identity();
		m.m[ 0] = float(2.0 / (r - l));
		m.m[ 5] = float(2.0 / (t - b));
		m.m[10] = float(-2.0 / (f - n));
		m.m[12] = float(-(r + l) / (r - l));
		m.m[13] = float(-(t + b) / (t - b));
		m.m[14] = float(-(f + n) / (f - n));
		MultCur(m);
	}

	void APIENTRY compat_glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
		Mat4 m = {};
		m.m[ 0] = float(2.0 * n / (r - l));
		m.m[ 5] = float(2.0 * n / (t - b));
		m.m[ 8] = float((r + l) / (r - l));
		m.m[ 9] = float((t + b) / (t - b));
		m.m[10] = float(-(f + n) / (f - n));
		m.m[11] = -1.0f;
		m.m[14] = float(-2.0 * f * n / (f - n));
		MultCur(m);
	}

	REAL(glActiveTexture);
	void APIENTRY compat_glActiveTexture(GLenum texture) { LIST(compat_glActiveTexture(texture)); activeTexUnit = int(texture - GL_TEXTURE0); real_glActiveTexture(texture); }

	// fog
	void APIENTRY compat_glFogfv(GLenum pname, const GLfloat* params) {
		switch (pname) {
			case GL_FOG_COLOR: std::memcpy(fog.color, params, sizeof(fog.color)); break;
			case GL_FOG_DENSITY: fog.density = params[0]; break;
			case GL_FOG_START: fog.start = params[0]; break;
			case GL_FOG_END: fog.end = params[0]; break;
			default: return;
		}
		fixedStateStamp++;
	}
	void APIENTRY compat_glFogf(GLenum pname, GLfloat param) { compat_glFogfv(pname, &param); }
	void APIENTRY compat_glFogi(GLenum pname, GLint param) { const float f = float(param); compat_glFogfv(pname, &f); }

	// lights (only what the engine's shaders read: colors and eye-space position)
	void APIENTRY compat_glLightfv(GLenum light, GLenum pname, const GLfloat* params) {
		const unsigned idx = light - GL_LIGHT0;
		if (idx >= 2)
			return;
		LightState& l = lights[idx];
		switch (pname) {
			case GL_AMBIENT: std::memcpy(l.ambient, params, sizeof(l.ambient)); break;
			case GL_DIFFUSE: std::memcpy(l.diffuse, params, sizeof(l.diffuse)); break;
			case GL_SPECULAR: std::memcpy(l.specular, params, sizeof(l.specular)); break;
			case GL_POSITION: {
				// stored in eye space: transformed by the modelview matrix current at this call
				const float* m = modelviewStack.Top().m;
				for (int row = 0; row < 4; row++)
					l.position[row] = m[0 + row] * params[0] + m[4 + row] * params[1] + m[8 + row] * params[2] + m[12 + row] * params[3];
			} break;
			default: return;
		}
		fixedStateStamp++;
	}

	// current color (used by the fixed-function path; kept for glGet and glPushAttrib)
	void SetColor(float r, float g, float b, float a) { LIST(SetColor(r, g, b, a)); currentColor[0] = r; currentColor[1] = g; currentColor[2] = b; currentColor[3] = a; fixedStateStamp++; }
	void APIENTRY compat_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { SetColor(r, g, b, a); }
	void APIENTRY compat_glColor3f(GLfloat r, GLfloat g, GLfloat b) { SetColor(r, g, b, 1.0f); }
	void APIENTRY compat_glColor4fv(const GLfloat* v) { SetColor(v[0], v[1], v[2], v[3]); }
	void APIENTRY compat_glColor3fv(const GLfloat* v) { SetColor(v[0], v[1], v[2], 1.0f); }
	void APIENTRY compat_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { SetColor(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
	void APIENTRY compat_glColor4ubv(const GLubyte* v) { SetColor(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f); }

	// ---- enable caps WebGL2 does not have ---------------------------------------------
	std::unordered_map<GLenum, bool> localCaps;

	bool IsNativeCap(GLenum cap) {
		switch (cap) {
			case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_DITHER:
			case GL_POLYGON_OFFSET_FILL: case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE:
			case GL_SCISSOR_TEST: case GL_STENCIL_TEST: case GL_RASTERIZER_DISCARD:
				return true;
			default:
				return false;
		}
	}

	REAL(glEnable);
	REAL(glDisable);
	REAL(glIsEnabled);
	void APIENTRY compat_glEnable(GLenum cap) { LIST(compat_glEnable(cap)); if (IsNativeCap(cap)) real_glEnable(cap); else localCaps[cap] = true; }
	void APIENTRY compat_glDisable(GLenum cap) { LIST(compat_glDisable(cap)); if (IsNativeCap(cap)) real_glDisable(cap); else localCaps[cap] = false; }
	GLboolean APIENTRY compat_glIsEnabled(GLenum cap) {
		if (IsNativeCap(cap))
			return real_glIsEnabled(cap);
		const auto it = localCaps.find(cap);
		// multisampling and dithering are on by default on desktop
		return (it != localCaps.end())? it->second: (cap == GL_MULTISAMPLE);
	}

	// ---- glGet for emulated state ---------------------------------------------------------
	REAL(glGetFloatv);
	REAL(glGetIntegerv);

	void APIENTRY compat_glGetFloatv(GLenum pname, GLfloat* data) {
		switch (pname) {
			case GL_MODELVIEW_MATRIX: std::memcpy(data, modelviewStack.Top().m, sizeof(Mat4)); return;
			case GL_PROJECTION_MATRIX: std::memcpy(data, projectionStack.Top().m, sizeof(Mat4)); return;
			case GL_TEXTURE_MATRIX: std::memcpy(data, textureStacks[std::min(activeTexUnit, MAX_TEX_UNITS - 1)].Top().m, sizeof(Mat4)); return;
			case GL_CURRENT_COLOR: std::memcpy(data, currentColor, sizeof(currentColor)); return;
			case GL_FOG_COLOR: std::memcpy(data, fog.color, sizeof(fog.color)); return;
			case GL_FOG_START: *data = fog.start; return;
			case GL_FOG_END: *data = fog.end; return;
			case GL_FOG_DENSITY: *data = fog.density; return;
			case GL_POINT_SIZE: *data = 1.0f; return;
			default: break;
		}
		real_glGetFloatv(pname, data);
	}

	void APIENTRY compat_glGetDoublev(GLenum pname, GLdouble* data) {
		float f[16] = {0};
		compat_glGetFloatv(pname, f);
		const int n = (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX)? 16: ((pname == GL_CURRENT_COLOR || pname == GL_VIEWPORT)? 4: 1);
		for (int i = 0; i < n; i++)
			data[i] = f[i];
	}

	void APIENTRY compat_glGetIntegerv(GLenum pname, GLint* data) {
		switch (pname) {
			case GL_MATRIX_MODE: *data = GLint(matrixMode); return;
			case GL_MODELVIEW_STACK_DEPTH: *data = GLint(modelviewStack.stack.size()); return;
			case GL_PROJECTION_STACK_DEPTH: *data = GLint(projectionStack.stack.size()); return;
			case GL_CLIENT_ACTIVE_TEXTURE: *data = GL_TEXTURE0 + clientActiveTexUnit; return;
			case GL_POLYGON_MODE: data[0] = data[1] = GL_FILL; return;
			case GL_SHADE_MODEL: *data = GL_SMOOTH; return;
			// nonzero while a display list is compiled; the engine defers texture uploads then
			case GL_LIST_INDEX: *data = GLint(WebGLCompat::GetCompilingList()); return;
			case GL_LIST_MODE: *data = GLint(WebGLCompat::GetCompilingListMode()); return;
			default: break;
		}
		real_glGetIntegerv(pname, data);
	}

	// ---- shaders and programs -------------------------------------------------------------
	struct ShaderInfo {
		bool isVertex = false;
		bool translated = false;
		std::string origSource;
		uint32_t varyingsUsed = 0;
		uint32_t attribsUsed = 0;
		int numFragOutputs = 0;
		std::vector<WebGLCompat::UniformDefault> uniformDefaults;
		// explicit layout(binding) of the cross-compiled source (GLSL ES 3.00 has none)
		std::vector<std::pair<std::string, int>> samplerBindings;
		std::vector<std::pair<std::string, int>> blockBindings;
	};
	struct ProgramInfo {
		std::vector<GLuint> shaders;
		std::vector<std::pair<std::string, int>> fragOutputBindings; // glBindFragDataLocation
		bool linked = false;
		bool hasBuiltins = false;
		uint32_t stateStamp = 0;
		GLint locModelView = -1, locProjection = -1, locMVP = -1, locNormal = -1;
		GLint locModelViewInv = -1, locProjectionInv = -1, locMVPInv = -1, locTexture = -1;
		GLint locFogColor = -1, locFogDensity = -1, locFogStart = -1, locFogEnd = -1, locFogScale = -1;
		GLint locLightAmbient[2] = {-1, -1}, locLightDiffuse[2] = {-1, -1}, locLightSpecular[2] = {-1, -1}, locLightPosition[2] = {-1, -1};
	};

	std::unordered_map<GLuint, ShaderInfo> shaders;
	std::unordered_map<GLuint, ProgramInfo> programs;
	GLuint currentProgram = 0;

	REAL(glCreateShader);
	REAL(glShaderSource);
	REAL(glCompileShader);
	REAL(glDeleteShader);
	REAL(glAttachShader);
	REAL(glDetachShader);
	REAL(glCreateProgram);
	REAL(glLinkProgram);
	REAL(glUseProgram);
	REAL(glDeleteProgram);
	REAL(glGetProgramiv);
	REAL(glGetUniformLocation);
	REAL(glBindAttribLocation);
	REAL(glUniformMatrix4fv);
	REAL(glUniformMatrix3fv);
	REAL(glUniform4fv);
	REAL(glUniform1f);
	REAL(glUniform1i);
	REAL(glUniform2fv);
	REAL(glUniform3fv);

	GLuint APIENTRY compat_glCreateShader(GLenum type) {
		// WebGL2 has vertex and fragment shaders only (no geometry/tessellation/compute)
		if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER)
			return 0;
		const GLuint id = real_glCreateShader(type);
		if (id != 0)
			shaders[id].isVertex = (type == GL_VERTEX_SHADER);
		return id;
	}

	void SetTranslatedSource(GLuint shader, ShaderInfo& info, uint32_t forceVaryings, const std::vector<std::pair<std::string, int>>* fragOutputBindings = nullptr) {
		const WebGLCompat::ShaderTranslation xl = WebGLCompat::TranslateShader(info.isVertex, info.origSource, forceVaryings, fragOutputBindings);
		info.numFragOutputs = xl.numFragOutputs;
		info.varyingsUsed = xl.varyingsUsed;
		info.attribsUsed = xl.attribsUsed;
		info.uniformDefaults = xl.uniformDefaults;
		info.samplerBindings.clear();
		info.blockBindings.clear();
		info.translated = true;

		// preferred: glslang + SPIRV-Cross on the same rewrites as desktop GLSL; the text
		// translation is the fallback when that fails
		if (WebGLCompat::CrossCompileAvailable()) {
			const WebGLCompat::ShaderTranslation dx = WebGLCompat::TranslateShader(info.isVertex, info.origSource, forceVaryings, fragOutputBindings, true);
			WebGLCompat::CrossCompiled cc;
			// diagnostics: RECOIL_WEB_GLSLANG=2 compiles but discards the result;
			// RECOIL_WEB_GLSLANG_LIMIT=N uses it for the first N shaders only
			static const bool discard = (std::getenv("RECOIL_WEB_GLSLANG") != nullptr && std::strcmp(std::getenv("RECOIL_WEB_GLSLANG"), "2") == 0);
			static const int limit = (std::getenv("RECOIL_WEB_GLSLANG_LIMIT") != nullptr)? std::atoi(std::getenv("RECOIL_WEB_GLSLANG_LIMIT")): -1;
			static int numUsed = 0;
			if (WebGLCompat::CrossCompile(info.isVertex, dx.source, cc) && !discard && (limit < 0 || numUsed < limit)) {
				if (limit >= 0 && ++numUsed == limit) {
					const std::string head = info.origSource.substr(0, std::min<size_t>(info.origSource.find_first_of('\n', info.origSource.find_first_not_of(" \t\r\n")), 120));
					LOG("[WebGLCompat] RECOIL_WEB_GLSLANG_LIMIT=%d reached at this %s shader (shader %u): %s", limit, info.isVertex? "vertex": "fragment", shader, head.c_str());
				}
				info.samplerBindings = std::move(cc.samplerBindings);
				info.blockBindings = std::move(cc.blockBindings);
				const GLchar* ptr = cc.source.c_str();
				real_glShaderSource(shader, 1, &ptr, nullptr);
				return;
			}
			static int numFailed = 0;
			if (++numFailed <= 12) {
				// first line of the original source usually names the shader well enough
				const std::string head = info.origSource.substr(0, std::min<size_t>(info.origSource.find_first_of('\n', info.origSource.find_first_not_of(" \t\r\n")), 120));
				LOG_L(L_WARNING, "[WebGLCompat] %s shader: cross-compilation failed, using the text translation (%s): %.600s",
					info.isVertex? "vertex": "fragment", head.c_str(), cc.log.c_str());
			}
			// RECOIL_WEB_TRACE: keep glslang's input for inspection (<cwd>/xc-fail-N.glsl)
			if (std::getenv("RECOIL_WEB_TRACE") != nullptr) {
				const std::string path = "xc-fail-" + std::to_string(numFailed) + ".glsl";
				if (FILE* f = fopen(path.c_str(), "wb"); f != nullptr) {
					fprintf(f, "// %s\n%s", cc.log.c_str(), dx.source.c_str());
					fclose(f);
				}
			}
		}
		const GLchar* ptr = xl.source.c_str();
		real_glShaderSource(shader, 1, &ptr, nullptr);
	}

	void APIENTRY compat_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths) {
		const auto it = shaders.find(shader);
		if (it == shaders.end()) {
			real_glShaderSource(shader, count, strings, lengths);
			return;
		}
		ShaderInfo& info = it->second;
		info.origSource.clear();
		for (GLsizei i = 0; i < count; i++) {
			if (strings[i] == nullptr)
				continue;
			if (lengths != nullptr && lengths[i] >= 0)
				info.origSource.append(strings[i], lengths[i]);
			else
				info.origSource.append(strings[i]);
		}
		SetTranslatedSource(shader, info, 0);
	}

	void APIENTRY compat_glDeleteShader(GLuint shader) { shaders.erase(shader); real_glDeleteShader(shader); }

	GLuint APIENTRY compat_glCreateProgram() {
		const GLuint id = real_glCreateProgram();
		if (id != 0)
			programs[id] = {};
		return id;
	}

	void APIENTRY compat_glAttachShader(GLuint program, GLuint shader) {
		if (shader == 0)
			return; // unsupported shader type (see compat_glCreateShader)
		if (const auto it = programs.find(program); it != programs.end())
			it->second.shaders.push_back(shader);
		real_glAttachShader(program, shader);
	}

	void APIENTRY compat_glDetachShader(GLuint program, GLuint shader) {
		if (shader == 0)
			return;
		if (const auto it = programs.find(program); it != programs.end())
			std::erase(it->second.shaders, shader);
		real_glDetachShader(program, shader);
	}

	void APIENTRY compat_glLinkProgram(GLuint program) {
		const auto pit = programs.find(program);
		if (pit == programs.end()) {
			real_glLinkProgram(program);
			return;
		}
		ProgramInfo& prog = pit->second;

		GLuint vsId = 0;
		GLuint fsId = 0;
		ShaderInfo* vs = nullptr;
		ShaderInfo* fs = nullptr;
		for (const GLuint id: prog.shaders) {
			const auto sit = shaders.find(id);
			if (sit == shaders.end())
				continue;
			if (sit->second.isVertex) { vs = &sit->second; vsId = id; } else { fs = &sit->second; fsId = id; }
		}

		// fragment outputs the engine placed with glBindFragDataLocation: GLSL ES only
		// knows layout(location = N), so the stage is translated again with them
		if (fs != nullptr && fs->translated && fs->numFragOutputs > 0 && !prog.fragOutputBindings.empty()) {
			SetTranslatedSource(fsId, *fs, 0, &prog.fragOutputBindings);
			real_glCompileShader(fsId);
		}

		// GLSL ES needs a vertex output for every fragment input: if the fragment stage reads
		// a compatibility varying the vertex stage never writes, declare it there as well
		if (vs != nullptr && fs != nullptr && vs->translated) {
			const uint32_t missing = fs->varyingsUsed & ~vs->varyingsUsed;
			if (missing != 0) {
				SetTranslatedSource(vsId, *vs, vs->varyingsUsed | missing);
				real_glCompileShader(vsId);
			}
		}

		// fixed-function vertex attributes at their conventional generic locations
		if (vs != nullptr) {
			static const char* const attribNames[16] = {
				"rcl_Vertex", nullptr, "rcl_Normal", "rcl_Color", nullptr, nullptr, nullptr, nullptr,
				"rcl_MultiTexCoord0", "rcl_MultiTexCoord1", "rcl_MultiTexCoord2", "rcl_MultiTexCoord3",
				"rcl_MultiTexCoord4", "rcl_MultiTexCoord5", "rcl_MultiTexCoord6", "rcl_MultiTexCoord7",
			};
			for (int loc = 0; loc < 16; loc++) {
				if ((vs->attribsUsed & (1u << loc)) != 0 && attribNames[loc] != nullptr)
					real_glBindAttribLocation(program, loc, attribNames[loc]);
			}
		}

		real_glLinkProgram(program);

		GLint ok = 0;
		real_glGetProgramiv(program, GL_LINK_STATUS, &ok);
		prog.linked = (ok != 0);
		prog.stateStamp = 0;
		prog.hasBuiltins = false;
		if (!prog.linked)
			return;

		// uniform initializers stripped by the translator
		real_glUseProgram(program);
		for (const ShaderInfo* sh: {vs, fs}) {
			if (sh == nullptr)
				continue;
			for (const WebGLCompat::UniformDefault& d: sh->uniformDefaults) {
				const GLint l = real_glGetUniformLocation(program, d.name.c_str());
				if (l < 0 || d.values.empty())
					continue;
				float v[4];
				for (int i = 0; i < 4; i++)
					v[i] = d.values[std::min<size_t>(i, d.values.size() - 1)];
				if (d.type == "float") real_glUniform1f(l, v[0]);
				else if (d.type == "int" || d.type == "bool") real_glUniform1i(l, int(v[0]));
				else if (d.type == "vec2") real_glUniform2fv(l, 1, v);
				else if (d.type == "vec3") real_glUniform3fv(l, 1, v);
				else if (d.type == "vec4") real_glUniform4fv(l, 1, v);
			}
			// layout(binding = N) of the original source, dropped by the GLSL ES 3.00 output
			for (const auto& [name, unit]: sh->samplerBindings) {
				const GLint l = real_glGetUniformLocation(program, name.c_str());
				if (l >= 0)
					real_glUniform1i(l, unit);
			}
			for (const auto& [name, binding]: sh->blockBindings) {
				const GLuint idx = glGetUniformBlockIndex(program, name.c_str());
				if (idx != GL_INVALID_INDEX)
					glUniformBlockBinding(program, idx, GLuint(binding));
			}
		}
		real_glUseProgram(currentProgram);

		const auto loc = [&](const char* name) {
			const GLint l = real_glGetUniformLocation(program, name);
			prog.hasBuiltins |= (l >= 0);
			return l;
		};
		prog.locModelView     = loc("rcl_ModelViewMatrix");
		prog.locProjection    = loc("rcl_ProjectionMatrix");
		prog.locMVP           = loc("rcl_ModelViewProjectionMatrix");
		prog.locNormal        = loc("rcl_NormalMatrix");
		prog.locModelViewInv  = loc("rcl_ModelViewMatrixInverse");
		prog.locProjectionInv = loc("rcl_ProjectionMatrixInverse");
		prog.locMVPInv        = loc("rcl_ModelViewProjectionMatrixInverse");
		prog.locTexture       = loc("rcl_TextureMatrix[0]");
		prog.locFogColor      = loc("rcl_Fog.color");
		prog.locFogDensity    = loc("rcl_Fog.density");
		prog.locFogStart      = loc("rcl_Fog.start");
		prog.locFogEnd        = loc("rcl_Fog.end");
		prog.locFogScale      = loc("rcl_Fog.scale");
		for (int i = 0; i < 2; i++) {
			const std::string base = "rcl_LightSource[" + std::to_string(i) + "].";
			prog.locLightAmbient[i]  = loc((base + "ambient").c_str());
			prog.locLightDiffuse[i]  = loc((base + "diffuse").c_str());
			prog.locLightSpecular[i] = loc((base + "specular").c_str());
			prog.locLightPosition[i] = loc((base + "position").c_str());
		}
	}

	void APIENTRY compat_glValidateProgram(GLuint program) {}

	void APIENTRY compat_glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
		if (pname == GL_VALIDATE_STATUS) {
			const auto it = programs.find(program);
			*params = (it != programs.end() && it->second.linked);
			return;
		}
		real_glGetProgramiv(program, pname, params);
	}

	void APIENTRY compat_glBindFragDataLocation(GLuint program, GLuint color, const GLchar* name) {
		if (const auto it = programs.find(program); it != programs.end() && name != nullptr)
			it->second.fragOutputBindings.emplace_back(name, int(color));
	}

	void APIENTRY compat_glUseProgram(GLuint program) { LIST(compat_glUseProgram(program)); currentProgram = program; real_glUseProgram(program); }

	void APIENTRY compat_glDeleteProgram(GLuint program) {
		programs.erase(program);
		if (currentProgram == program)
			currentProgram = 0;
		real_glDeleteProgram(program);
	}

	// Emscripten's glGetUniformLocation throws on a program that failed to link
	GLint APIENTRY compat_glGetUniformLocation(GLuint program, const GLchar* name) {
		const auto it = programs.find(program);
		if (it == programs.end() || !it->second.linked)
			return -1;
		return real_glGetUniformLocation(program, name);
	}

	// upload the fixed-function state the current program's shaders read
	void FlushFixedState() {
		if (currentProgram == 0)
			return;
		const auto it = programs.find(currentProgram);
		if (it == programs.end())
			return;
		ProgramInfo& p = it->second;
		if (!p.hasBuiltins || p.stateStamp == fixedStateStamp)
			return;
		p.stateStamp = fixedStateStamp;

		const Mat4& mv = modelviewStack.Top();
		const Mat4& pr = projectionStack.Top();

		if (p.locModelView >= 0) real_glUniformMatrix4fv(p.locModelView, 1, GL_FALSE, mv.m);
		if (p.locProjection >= 0) real_glUniformMatrix4fv(p.locProjection, 1, GL_FALSE, pr.m);
		if (p.locMVP >= 0 || p.locMVPInv >= 0) {
			const Mat4 mvp = Mul(pr, mv);
			if (p.locMVP >= 0) real_glUniformMatrix4fv(p.locMVP, 1, GL_FALSE, mvp.m);
			if (p.locMVPInv >= 0) real_glUniformMatrix4fv(p.locMVPInv, 1, GL_FALSE, Inverse(mvp).m);
		}
		if (p.locModelViewInv >= 0 || p.locNormal >= 0) {
			const Mat4 inv = Inverse(mv);
			if (p.locModelViewInv >= 0) real_glUniformMatrix4fv(p.locModelViewInv, 1, GL_FALSE, inv.m);
			if (p.locNormal >= 0) {
				// transpose of the inverse's upper 3x3
				const float n[9] = {inv.m[0], inv.m[4], inv.m[8],  inv.m[1], inv.m[5], inv.m[9],  inv.m[2], inv.m[6], inv.m[10]};
				real_glUniformMatrix3fv(p.locNormal, 1, GL_FALSE, n);
			}
		}
		if (p.locProjectionInv >= 0) real_glUniformMatrix4fv(p.locProjectionInv, 1, GL_FALSE, Inverse(pr).m);
		if (p.locTexture >= 0) {
			float tex[4 * 16];
			for (int i = 0; i < 4; i++)
				std::memcpy(tex + i * 16, textureStacks[i].Top().m, sizeof(Mat4));
			real_glUniformMatrix4fv(p.locTexture, 4, GL_FALSE, tex);
		}
		if (p.locFogColor >= 0) real_glUniform4fv(p.locFogColor, 1, fog.color);
		if (p.locFogDensity >= 0) real_glUniform1f(p.locFogDensity, fog.density);
		if (p.locFogStart >= 0) real_glUniform1f(p.locFogStart, fog.start);
		if (p.locFogEnd >= 0) real_glUniform1f(p.locFogEnd, fog.end);
		if (p.locFogScale >= 0) real_glUniform1f(p.locFogScale, (fog.end != fog.start)? 1.0f / (fog.end - fog.start): 0.0f);
		for (int i = 0; i < 2; i++) {
			if (p.locLightAmbient[i] >= 0) real_glUniform4fv(p.locLightAmbient[i], 1, lights[i].ambient);
			if (p.locLightDiffuse[i] >= 0) real_glUniform4fv(p.locLightDiffuse[i], 1, lights[i].diffuse);
			if (p.locLightSpecular[i] >= 0) real_glUniform4fv(p.locLightSpecular[i], 1, lights[i].specular);
			if (p.locLightPosition[i] >= 0) real_glUniform4fv(p.locLightPosition[i], 1, lights[i].position);
		}
	}

	// ---- client-state vertex arrays -> generic attributes -----------------------------------
	REAL(glEnableVertexAttribArray);
	REAL(glDisableVertexAttribArray);
	REAL(glVertexAttribPointer);
	REAL(glVertexAttribIPointer);
	REAL(glVertexAttribDivisor);
	REAL(glBindVertexArray);
	REAL(glDeleteVertexArrays);

	// shadow of the vertex attribute state per VAO, for display-list draw snapshots:
	// Emscripten's client-array emulation can't report client pointers, and its
	// glGetVertexAttrib* warns on every enabled attribute
	using AttribShadow = WebGLCompat::AttribShadow;
	std::unordered_map<GLuint, std::array<AttribShadow, WebGLCompat::MAX_SHADOW_ATTRIBS>> vaoAttribs;
	GLuint boundVAO = 0;

	AttribShadow* Shadow(GLuint index) {
		if (index >= WebGLCompat::MAX_SHADOW_ATTRIBS)
			return nullptr;
		return &vaoAttribs[boundVAO][index];
	}
	void ShadowPointer(GLuint index, GLint size, GLenum type, bool normalized, bool integer, GLsizei stride, const void* ptr) {
		AttribShadow* s = Shadow(index);
		if (s == nullptr)
			return;
		GLint buf = 0;
		real_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
		s->size = size; s->type = type; s->normalized = normalized; s->integer = integer;
		s->stride = stride; s->buffer = GLuint(buf); s->ptr = reinterpret_cast<uintptr_t>(ptr);
	}

	void APIENTRY compat_glEnableVertexAttribArray(GLuint index) { if (AttribShadow* s = Shadow(index)) s->enabled = true; real_glEnableVertexAttribArray(index); }
	void APIENTRY compat_glDisableVertexAttribArray(GLuint index) { if (AttribShadow* s = Shadow(index)) s->enabled = false; real_glDisableVertexAttribArray(index); }
	void APIENTRY compat_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* ptr) {
		ShadowPointer(index, size, type, normalized != GL_FALSE, false, stride, ptr);
		real_glVertexAttribPointer(index, size, type, normalized, stride, ptr);
	}
	void APIENTRY compat_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* ptr) {
		ShadowPointer(index, size, type, false, true, stride, ptr);
		real_glVertexAttribIPointer(index, size, type, stride, ptr);
	}
	void APIENTRY compat_glVertexAttribDivisor(GLuint index, GLuint divisor) { if (AttribShadow* s = Shadow(index)) s->divisor = divisor; real_glVertexAttribDivisor(index, divisor); }
	void APIENTRY compat_glBindVertexArray(GLuint vao) { boundVAO = vao; real_glBindVertexArray(vao); }
	void APIENTRY compat_glDeleteVertexArrays(GLsizei n, const GLuint* vaos) {
		for (GLsizei i = 0; i < n; i++) {
			if (vaos[i] == 0)
				continue;
			vaoAttribs.erase(vaos[i]);
			if (vaos[i] == boundVAO)
				boundVAO = 0;
		}
		real_glDeleteVertexArrays(n, vaos);
	}

	GLuint ClientArrayToAttrib(GLenum array) {
		switch (array) {
			case GL_VERTEX_ARRAY: return WebGLCompat::ATTRIB_VERTEX;
			case GL_NORMAL_ARRAY: return WebGLCompat::ATTRIB_NORMAL;
			case GL_COLOR_ARRAY: return WebGLCompat::ATTRIB_COLOR;
			case GL_TEXTURE_COORD_ARRAY: return WebGLCompat::ATTRIB_TEXCOORD0 + clientActiveTexUnit;
			default: return ~0u;
		}
	}
	bool IsIntType(GLenum type) { return (type != GL_FLOAT && type != GL_HALF_FLOAT); }

	void APIENTRY compat_glEnableClientState(GLenum array) { if (const GLuint a = ClientArrayToAttrib(array); a != ~0u) compat_glEnableVertexAttribArray(a); }
	void APIENTRY compat_glDisableClientState(GLenum array) { if (const GLuint a = ClientArrayToAttrib(array); a != ~0u) compat_glDisableVertexAttribArray(a); }
	void APIENTRY compat_glClientActiveTexture(GLenum texture) { clientActiveTexUnit = std::clamp(int(texture - GL_TEXTURE0), 0, MAX_TEX_UNITS - 1); }
	void APIENTRY compat_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* ptr) { compat_glVertexAttribPointer(WebGLCompat::ATTRIB_VERTEX, size, type, GL_FALSE, stride, ptr); }
	void APIENTRY compat_glNormalPointer(GLenum type, GLsizei stride, const void* ptr) { compat_glVertexAttribPointer(WebGLCompat::ATTRIB_NORMAL, 3, type, IsIntType(type), stride, ptr); }
	void APIENTRY compat_glColorPointer(GLint size, GLenum type, GLsizei stride, const void* ptr) { compat_glVertexAttribPointer(WebGLCompat::ATTRIB_COLOR, size, type, IsIntType(type), stride, ptr); }
	void APIENTRY compat_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* ptr) { compat_glVertexAttribPointer(WebGLCompat::ATTRIB_TEXCOORD0 + clientActiveTexUnit, size, type, GL_FALSE, stride, ptr); }

	// ---- draw calls ------------------------------------------------------------------------
	REAL(glDrawArrays);
	REAL(glDrawElements);
	REAL(glDrawRangeElements);
	REAL(glDrawArraysInstanced);
	REAL(glDrawElementsInstanced);
	REAL(glGenBuffers);
	REAL(glBindBuffer);
	REAL(glBufferData);

	uint32_t skippedQuadElementDraws = 0;

	// GL_QUADS as indexed triangles: a shared index buffer with (0 1 2, 0 2 3) per quad
	GLuint quadIndexBuffer = 0;
	GLsizei quadIndexCapacity = 0; // in quads

	void DrawQuadArrays(GLint first, GLsizei count, GLsizei instances) {
		const GLsizei numQuads = count / 4;
		if (numQuads <= 0)
			return;

		GLint prevElemBuf = 0;
		real_glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);

		if (quadIndexBuffer == 0)
			real_glGenBuffers(1, &quadIndexBuffer);
		real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadIndexBuffer);

		const GLsizei neededQuads = first / 4 + numQuads + ((first % 4 != 0)? 1: 0);
		if (first % 4 != 0 || neededQuads > quadIndexCapacity) {
			// (re)build; a draw not starting on a quad boundary gets a one-off buffer
			const GLsizei base = (first % 4 != 0)? first: 0;
			const GLsizei quads = (first % 4 != 0)? numQuads: std::max(neededQuads, 4096);
			std::vector<GLuint> idx(size_t(quads) * 6);
			for (GLsizei q = 0; q < quads; q++) {
				const GLuint v = base + q * 4;
				GLuint* o = &idx[size_t(q) * 6];
				o[0] = v; o[1] = v + 1; o[2] = v + 2;  o[3] = v; o[4] = v + 2; o[5] = v + 3;
			}
			real_glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(GLuint), idx.data(), GL_DYNAMIC_DRAW);
			quadIndexCapacity = (first % 4 != 0)? 0: quads;
		}

		const void* offset = (first % 4 != 0)? nullptr: reinterpret_cast<const void*>(uintptr_t(first / 4) * 6 * sizeof(GLuint));
		if (instances > 0)
			real_glDrawElementsInstanced(GL_TRIANGLES, numQuads * 6, GL_UNSIGNED_INT, offset, instances);
		else
			real_glDrawElements(GL_TRIANGLES, numQuads * 6, GL_UNSIGNED_INT, offset);

		real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GLuint(prevElemBuf));
	}

	// with no shader bound the draw goes through the fixed-function emulation program
	struct FixedFunctionScope {
		FixedFunctionScope(): active(currentProgram == 0 && WebGLCompat::BeginFixedFunctionDraw(false)) {}
		~FixedFunctionScope() { if (active) WebGLCompat::EndFixedFunctionDraw(); }
		const bool active;
	};

	// desktop-only primitive modes with a direct WebGL equivalent
	GLenum MapMode(GLenum mode) {
		switch (mode) {
			case GL_QUAD_STRIP: return GL_TRIANGLE_STRIP;
			case GL_POLYGON: return GL_TRIANGLE_FAN;
			default: return mode;
		}
	}

	void APIENTRY compat_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
		if (WebGLCompat::listCompiling) { WebGLCompat::ListRecordDraw(mode, first, count, 0, nullptr, 0); return; }
		const FixedFunctionScope ffp;
		FlushFixedState();
		if (mode == GL_QUADS) { DrawQuadArrays(first, count, 0); return; }
		real_glDrawArrays(MapMode(mode), first, count);
	}
	void APIENTRY compat_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
		if (WebGLCompat::listCompiling) { WebGLCompat::ListRecordDraw(mode, first, count, 0, nullptr, instances); return; }
		const FixedFunctionScope ffp;
		FlushFixedState();
		if (mode == GL_QUADS) { DrawQuadArrays(first, count, instances); return; }
		real_glDrawArraysInstanced(MapMode(mode), first, count, instances);
	}
	void APIENTRY compat_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
		if (WebGLCompat::listCompiling) { WebGLCompat::ListRecordDraw(mode, 0, count, type, indices, 0); return; }
		const FixedFunctionScope ffp;
		FlushFixedState();
		if (mode == GL_QUADS) { skippedQuadElementDraws++; return; }
		real_glDrawElements(MapMode(mode), count, type, indices);
	}
	void APIENTRY compat_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
		if (WebGLCompat::listCompiling) { WebGLCompat::ListRecordDraw(mode, 0, count, type, indices, 0); return; }
		const FixedFunctionScope ffp;
		FlushFixedState();
		if (mode == GL_QUADS) { skippedQuadElementDraws++; return; }
		real_glDrawRangeElements(MapMode(mode), start, end, count, type, indices);
	}
	void APIENTRY compat_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances) {
		if (WebGLCompat::listCompiling) { WebGLCompat::ListRecordDraw(mode, 0, count, type, indices, instances); return; }
		const FixedFunctionScope ffp;
		FlushFixedState();
		if (mode == GL_QUADS) { skippedQuadElementDraws++; return; }
		real_glDrawElementsInstanced(MapMode(mode), count, type, indices, instances);
	}
	void APIENTRY compat_glMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
		for (GLsizei i = 0; i < drawcount; i++)
			compat_glDrawArrays(mode, first[i], count[i]);
	}
	void APIENTRY compat_glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices, GLsizei drawcount) {
		for (GLsizei i = 0; i < drawcount; i++)
			compat_glDrawElements(mode, count[i], type, indices[i]);
	}

	// ---- glPushAttrib / glPopAttrib ---------------------------------------------------------
	REAL(glBlendFuncSeparate);
	REAL(glBlendEquationSeparate);
	REAL(glColorMask);
	REAL(glDepthFunc);
	REAL(glDepthMask);
	REAL(glCullFace);
	REAL(glFrontFace);
	REAL(glPolygonOffset);
	REAL(glViewport);
	REAL(glScissor);
	REAL(glLineWidth);
	REAL(glStencilMask);
	REAL(glGetBooleanv);
	void APIENTRY compat_glGetBooleanv(GLenum pname, GLboolean* data) {
		if (pname == GL_LIST_INDEX) { *data = (WebGLCompat::GetCompilingList() != 0)? GL_TRUE: GL_FALSE; return; }
		real_glGetBooleanv(pname, data);
	}

	struct AttribState {
		GLbitfield mask = 0;
		// enable bits
		bool blend, cullFace, depthTest, polyOffsetFill, scissorTest, stencilTest;
		std::unordered_map<GLenum, bool> local;
		// color buffer
		GLint blendSrcRGB, blendDstRGB, blendSrcA, blendDstA, blendEqRGB, blendEqA;
		GLboolean colorMask[4];
		// depth buffer
		GLint depthFunc;
		GLboolean depthMask;
		// polygon
		GLint cullFaceMode, frontFace;
		GLfloat polyOffsetFactor, polyOffsetUnits;
		// viewport, scissor, line, stencil, current, transform
		GLint viewport[4], scissorBox[4];
		GLfloat lineWidth;
		GLint stencilMask;
		float color[4];
		GLenum matrixMode;
	};
	std::vector<AttribState> attribStack;

	void APIENTRY compat_glPushAttrib(GLbitfield mask) {
		LIST(compat_glPushAttrib(mask));
		AttribState s;
		s.mask = mask;
		// the enable flags belong to their own groups as well as to GL_ENABLE_BIT
		s.blend = real_glIsEnabled(GL_BLEND);
		s.cullFace = real_glIsEnabled(GL_CULL_FACE);
		s.depthTest = real_glIsEnabled(GL_DEPTH_TEST);
		s.polyOffsetFill = real_glIsEnabled(GL_POLYGON_OFFSET_FILL);
		s.scissorTest = real_glIsEnabled(GL_SCISSOR_TEST);
		s.stencilTest = real_glIsEnabled(GL_STENCIL_TEST);
		if (mask & GL_ENABLE_BIT)
			s.local = localCaps;
		if (mask & GL_COLOR_BUFFER_BIT) {
			real_glGetIntegerv(GL_BLEND_SRC_RGB, &s.blendSrcRGB);
			real_glGetIntegerv(GL_BLEND_DST_RGB, &s.blendDstRGB);
			real_glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendSrcA);
			real_glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendDstA);
			real_glGetIntegerv(GL_BLEND_EQUATION_RGB, &s.blendEqRGB);
			real_glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blendEqA);
			real_glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
		}
		if (mask & GL_DEPTH_BUFFER_BIT) {
			real_glGetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
			real_glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
		}
		if (mask & GL_POLYGON_BIT) {
			real_glGetIntegerv(GL_CULL_FACE_MODE, &s.cullFaceMode);
			real_glGetIntegerv(GL_FRONT_FACE, &s.frontFace);
			real_glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &s.polyOffsetFactor);
			real_glGetFloatv(GL_POLYGON_OFFSET_UNITS, &s.polyOffsetUnits);
		}
		if (mask & GL_VIEWPORT_BIT)
			real_glGetIntegerv(GL_VIEWPORT, s.viewport);
		if (mask & GL_SCISSOR_BIT)
			real_glGetIntegerv(GL_SCISSOR_BOX, s.scissorBox);
		if (mask & GL_LINE_BIT)
			real_glGetFloatv(GL_LINE_WIDTH, &s.lineWidth);
		if (mask & GL_STENCIL_BUFFER_BIT)
			real_glGetIntegerv(GL_STENCIL_WRITEMASK, &s.stencilMask);
		if (mask & GL_CURRENT_BIT)
			std::memcpy(s.color, currentColor, sizeof(s.color));
		if (mask & GL_TRANSFORM_BIT)
			s.matrixMode = matrixMode;
		attribStack.push_back(std::move(s));
	}

	void SetCap(GLenum cap, bool on) { if (on) real_glEnable(cap); else real_glDisable(cap); }

	void APIENTRY compat_glPopAttrib() {
		LIST(compat_glPopAttrib());
		if (attribStack.empty())
			return;
		const AttribState s = std::move(attribStack.back());
		attribStack.pop_back();
		const GLbitfield mask = s.mask;

		if (mask & GL_ENABLE_BIT) {
			localCaps = s.local;
			SetCap(GL_BLEND, s.blend);
			SetCap(GL_CULL_FACE, s.cullFace);
			SetCap(GL_DEPTH_TEST, s.depthTest);
			SetCap(GL_POLYGON_OFFSET_FILL, s.polyOffsetFill);
			SetCap(GL_SCISSOR_TEST, s.scissorTest);
			SetCap(GL_STENCIL_TEST, s.stencilTest);
		}
		if (mask & GL_COLOR_BUFFER_BIT) {
			SetCap(GL_BLEND, s.blend);
			real_glBlendFuncSeparate(s.blendSrcRGB, s.blendDstRGB, s.blendSrcA, s.blendDstA);
			real_glBlendEquationSeparate(s.blendEqRGB, s.blendEqA);
			real_glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
		}
		if (mask & GL_DEPTH_BUFFER_BIT) {
			SetCap(GL_DEPTH_TEST, s.depthTest);
			real_glDepthFunc(s.depthFunc);
			real_glDepthMask(s.depthMask);
		}
		if (mask & GL_POLYGON_BIT) {
			SetCap(GL_CULL_FACE, s.cullFace);
			SetCap(GL_POLYGON_OFFSET_FILL, s.polyOffsetFill);
			real_glCullFace(s.cullFaceMode);
			real_glFrontFace(s.frontFace);
			real_glPolygonOffset(s.polyOffsetFactor, s.polyOffsetUnits);
		}
		if (mask & GL_VIEWPORT_BIT)
			real_glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
		if (mask & GL_SCISSOR_BIT) {
			SetCap(GL_SCISSOR_TEST, s.scissorTest);
			real_glScissor(s.scissorBox[0], s.scissorBox[1], s.scissorBox[2], s.scissorBox[3]);
		}
		if (mask & GL_LINE_BIT)
			real_glLineWidth(s.lineWidth);
		if (mask & GL_STENCIL_BUFFER_BIT) {
			SetCap(GL_STENCIL_TEST, s.stencilTest);
			real_glStencilMask(s.stencilMask);
		}
		if (mask & GL_CURRENT_BIT)
			SetColor(s.color[0], s.color[1], s.color[2], s.color[3]);
		if (mask & GL_TRANSFORM_BIT)
			matrixMode = s.matrixMode;
	}

	// ---- misc desktop entry points with trivial WebGL2 answers --------------------------------
	REAL(glClearDepthf);
	REAL(glDepthRangef);
	REAL(glDrawBuffers);
	void APIENTRY compat_glClearDepth(GLdouble d) { real_glClearDepthf(float(d)); }
	void APIENTRY compat_glDepthRange(GLdouble n, GLdouble f) { real_glDepthRangef(float(n), float(f)); }

	// glDrawBuffer(single buffer) -> glDrawBuffers; the default framebuffer only has BACK
	void APIENTRY compat_glDrawBuffer(GLenum buf) {
		GLint fbo = 0;
		real_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
		if (fbo == 0) {
			const GLenum b = (buf == GL_NONE)? GL_NONE: GL_BACK;
			real_glDrawBuffers(1, &b);
			return;
		}
		if (buf == GL_NONE || (buf >= GL_COLOR_ATTACHMENT0 && buf <= GL_COLOR_ATTACHMENT15)) {
			// WebGL2 requires buffer i to be COLOR_ATTACHMENTi or NONE
			GLenum bufs[16];
			const int n = (buf == GL_NONE)? 1: int(buf - GL_COLOR_ATTACHMENT0) + 1;
			for (int i = 0; i < n; i++)
				bufs[i] = GL_NONE;
			if (buf != GL_NONE)
				bufs[n - 1] = buf;
			real_glDrawBuffers(n, bufs);
		}
	}
}


void WebGLCompat::InstallStateEmulation()
{
	// matrices, fog, lights, color: fully emulated (the real pointers are no-op stubs)
	#define EMULATE(name) glad_##name = compat_##name
	EMULATE(glMatrixMode); EMULATE(glLoadIdentity); EMULATE(glLoadMatrixf); EMULATE(glLoadMatrixd);
	EMULATE(glMultMatrixf); EMULATE(glMultMatrixd); EMULATE(glPushMatrix); EMULATE(glPopMatrix);
	EMULATE(glTranslatef); EMULATE(glTranslated); EMULATE(glScalef); EMULATE(glScaled);
	EMULATE(glRotatef); EMULATE(glRotated); EMULATE(glOrtho); EMULATE(glFrustum);
	EMULATE(glFogfv); EMULATE(glFogf); EMULATE(glFogi); EMULATE(glLightfv);
	EMULATE(glColor4f); EMULATE(glColor3f); EMULATE(glColor4fv); EMULATE(glColor3fv); EMULATE(glColor4ub); EMULATE(glColor4ubv);
	EMULATE(glGetDoublev);
	EMULATE(glEnableClientState); EMULATE(glDisableClientState); EMULATE(glClientActiveTexture);
	EMULATE(glVertexPointer); EMULATE(glNormalPointer); EMULATE(glColorPointer); EMULATE(glTexCoordPointer);
	EMULATE(glMultiDrawArrays); EMULATE(glMultiDrawElements);
	EMULATE(glPushAttrib); EMULATE(glPopAttrib);
	EMULATE(glClearDepth); EMULATE(glDepthRange); EMULATE(glDrawBuffer);
	EMULATE(glBindFragDataLocation);
	#undef EMULATE

	HOOK(glActiveTexture);
	HOOK(glEnable); HOOK(glDisable); HOOK(glIsEnabled);
	HOOK(glGetFloatv); HOOK(glGetIntegerv);

	HOOK(glCreateShader); HOOK(glShaderSource); HOOK(glDeleteShader);
	HOOK(glAttachShader); HOOK(glDetachShader);
	HOOK(glCreateProgram); HOOK(glLinkProgram); HOOK(glUseProgram); HOOK(glDeleteProgram);
	HOOK(glGetUniformLocation); HOOK(glGetProgramiv);
	glad_glValidateProgram = compat_glValidateProgram;

	HOOK(glEnableVertexAttribArray); HOOK(glDisableVertexAttribArray);
	HOOK(glVertexAttribPointer); HOOK(glVertexAttribIPointer); HOOK(glVertexAttribDivisor);
	HOOK(glBindVertexArray); HOOK(glDeleteVertexArrays);
	HOOK(glGetBooleanv);

	HOOK(glDrawArrays); HOOK(glDrawElements); HOOK(glDrawRangeElements);
	HOOK(glDrawArraysInstanced); HOOK(glDrawElementsInstanced);

	// real WebGL2 functions the emulation calls
	#define KEEP(name) real_##name = glad_##name
	KEEP(glCompileShader); KEEP(glBindAttribLocation);
	KEEP(glUniformMatrix4fv); KEEP(glUniformMatrix3fv); KEEP(glUniform4fv); KEEP(glUniform1f); KEEP(glUniform1i); KEEP(glUniform2fv); KEEP(glUniform3fv);
	KEEP(glGenBuffers); KEEP(glBindBuffer); KEEP(glBufferData);
	KEEP(glBlendFuncSeparate); KEEP(glBlendEquationSeparate); KEEP(glColorMask);
	KEEP(glDepthFunc); KEEP(glDepthMask); KEEP(glCullFace); KEEP(glFrontFace); KEEP(glPolygonOffset);
	KEEP(glViewport); KEEP(glScissor); KEEP(glLineWidth); KEEP(glStencilMask);
	KEEP(glClearDepthf); KEEP(glDepthRangef); KEEP(glDrawBuffers);
	#undef KEEP
}

uint32_t WebGLCompat::GetSkippedDrawCount() { return skippedQuadElementDraws; }

const float* WebGLCompat::GetCurrentColor() { return currentColor; }
bool WebGLCompat::HasCurrentProgram() { return (currentProgram != 0); }
bool WebGLCompat::IsLocalCapEnabled(GLenum cap) { const auto it = localCaps.find(cap); return (it != localCaps.end() && it->second); }
void WebGLCompat::FlushFixedStateForDraw() { FlushFixedState(); }

void WebGLCompat::GetFixedMatrices(float* mvp, float* tex0)
{
	std::memcpy(mvp, Mul(projectionStack.Top(), modelviewStack.Top()).m, sizeof(Mat4));
	std::memcpy(tex0, textureStacks[0].Top().m, sizeof(Mat4));
}

void WebGLCompat::RawDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	if (mode == GL_QUADS) { DrawQuadArrays(first, count, 0); return; }
	real_glDrawArrays(MapMode(mode), first, count);
}

const WebGLCompat::AttribShadow* WebGLCompat::GetAttribShadow(GLuint index) { return Shadow(index); }

void WebGLCompat::RawDraw(GLenum mode, GLint first, GLsizei count, bool indexed, GLsizei instances)
{
	// indexed: GL_UNSIGNED_INT indices at offset 0 of the bound element buffer, already
	// converted from GL_QUADS to triangles
	if (indexed) {
		if (instances > 0)
			real_glDrawElementsInstanced(MapMode(mode), count, GL_UNSIGNED_INT, nullptr, instances);
		else
			real_glDrawElements(MapMode(mode), count, GL_UNSIGNED_INT, nullptr);
		return;
	}
	if (mode == GL_QUADS) { DrawQuadArrays(first, count, instances); return; }
	if (instances > 0)
		real_glDrawArraysInstanced(MapMode(mode), first, count, instances);
	else
		real_glDrawArrays(MapMode(mode), first, count);
}

void WebGLCompat::RawDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
	if (mode == GL_QUADS) { skippedQuadElementDraws++; return; }
	real_glDrawElements(MapMode(mode), count, type, indices);
}

void WebGLCompat::RawUseProgram(GLuint program) { real_glUseProgram(program); }
GLuint WebGLCompat::GetCurrentProgram() { return currentProgram; }
