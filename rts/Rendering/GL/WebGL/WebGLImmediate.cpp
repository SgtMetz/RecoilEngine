/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Immediate mode, display lists and the fixed-function pipeline for the WebGL2 shim
// (see WebGLCompat.h). Lua UI code (gl.BeginEnd, gl.CreateList, gl.Rect, gl.TexRect, ...)
// and a few engine paths still draw this way.
//  - glBegin .. glEnd collects vertices and draws them from a streaming buffer
//  - draws issued while no shader is bound use a small emulation program (vertex color,
//    one optional texture, alpha test) written against the gl_* built-ins, so the
//    translator and the fixed-function state upload serve it like any engine shader
//  - display lists are recorded as closures of the emulated entry points; draw calls in a
//    list copy the vertex and index data they read into a private VAO at record time

#include "WebGLInternal.h"
#include "WebGLShaderTranslate.h"

#include <glad/glad.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "System/Log/ILog.h"

#include <emscripten/html5.h>

bool WebGLCompat::listCompiling = false;
int WebGLCompat::traceDraws = 0;

namespace {
	using WebGLCompat::listCompiling;
	using WebGLCompat::ListRecord;

	#define REAL(name) decltype(glad_##name) real_##name = nullptr
	#define HOOK(name) do { real_##name = glad_##name; glad_##name = compat_##name; } while (0)

	// ---- immediate mode -----------------------------------------------------------------
	struct Vertex {
		float pos[4];
		float color[4];
		float tex[4];
		float normal[3];
	};

	GLenum batchMode = 0;
	bool inBeginEnd = false;
	std::vector<Vertex> batch;
	// vertices emitted in a display list before the list set any color take the color
	// that is current when the list runs
	size_t batchInheritColor = 0;

	float immColor[4] = {1, 1, 1, 1};
	bool immColorKnown = true; // false inside a list until it calls glColor
	float immTex[4] = {0, 0, 0, 1};
	float immNormal[3] = {0, 0, 1};

	GLuint immVAO = 0;
	GLuint immVBO = 0;

	// ---- fixed-function program ------------------------------------------------------------
	GLuint ffpProgram = 0;
	bool ffpFailed = false;
	GLint ffpLocTexMode = -1, ffpLocAlphaMode = -1, ffpLocAlphaRef = -1, ffpLocPointSize = -1;
	GLenum alphaFunc = GL_ALWAYS;
	float alphaRef = 0.0f;
	float pointSize = 1.0f;

	const char* const ffpVertSrc = R"(#version 120
varying vec4 vColor;
varying vec2 vTex;
uniform float uPointSize;
void main() {
	gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
	vColor = gl_Color;
	vTex = (gl_TextureMatrix[0] * gl_MultiTexCoord0).xy;
	gl_PointSize = uPointSize;
}
)";
	const char* const ffpFragSrc = R"(#version 120
varying vec4 vColor;
varying vec2 vTex;
uniform sampler2D uTex;
uniform int uTexMode;    // 0: untextured, 1: modulate with uTex
uniform int uAlphaMode;  // 0: off, 1: >, 2: >=, 3: <, 4: <=
uniform float uAlphaRef;
void main() {
	vec4 c = vColor;
	if (uTexMode == 1)
		c *= texture2D(uTex, vTex);
	if (uAlphaMode == 1 && !(c.a >  uAlphaRef)) discard;
	if (uAlphaMode == 2 && !(c.a >= uAlphaRef)) discard;
	if (uAlphaMode == 3 && !(c.a <  uAlphaRef)) discard;
	if (uAlphaMode == 4 && !(c.a <= uAlphaRef)) discard;
	gl_FragColor = c;
}
)";

	// built through the hooked entry points, so it is translated and tracked like any program
	bool CreateFixedFunctionProgram() {
		if (ffpProgram != 0)
			return true;
		if (ffpFailed)
			return false;

		const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
		const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(vs, 1, &ffpVertSrc, nullptr);
		glShaderSource(fs, 1, &ffpFragSrc, nullptr);
		glCompileShader(vs);
		glCompileShader(fs);

		const GLuint prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);

		GLint ok = 0;
		glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (!ok) {
			char log[1024] = {0};
			glGetShaderInfoLog(vs, sizeof(log) - 1, nullptr, log);
			LOG_L(L_ERROR, "[WebGLCompat] fixed-function program: vertex shader log: %s", log);
			glGetShaderInfoLog(fs, sizeof(log) - 1, nullptr, log);
			LOG_L(L_ERROR, "[WebGLCompat] fixed-function program: fragment shader log: %s", log);
			glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
			LOG_L(L_ERROR, "[WebGLCompat] fixed-function program: link log: %s", log);
			ffpFailed = true;
			return false;
		}

		ffpProgram = prog;
		ffpLocTexMode = glGetUniformLocation(prog, "uTexMode");
		ffpLocAlphaMode = glGetUniformLocation(prog, "uAlphaMode");
		ffpLocAlphaRef = glGetUniformLocation(prog, "uAlphaRef");
		ffpLocPointSize = glGetUniformLocation(prog, "uPointSize");

		glUseProgram(prog);
		glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
		glUseProgram(0);
		return true;
	}

	int AlphaMode() {
		if (!WebGLCompat::IsLocalCapEnabled(GL_ALPHA_TEST))
			return 0;
		switch (alphaFunc) {
			case GL_GREATER: return 1;
			case GL_GEQUAL: return 2;
			case GL_LESS: return 3;
			case GL_LEQUAL: return 4;
			default: return 0;
		}
	}

	void DrawBatch(GLenum mode, std::vector<Vertex>& verts, size_t inheritColor) {
		if (verts.empty())
			return;

		const float* curColor = WebGLCompat::GetCurrentColor();
		for (size_t i = 0; i < inheritColor && i < verts.size(); i++)
			std::memcpy(verts[i].color, curColor, sizeof(verts[i].color));

		GLint prevVAO = 0;
		GLint prevArrayBuf = 0;
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);

		if (immVAO == 0) {
			glGenVertexArrays(1, &immVAO);
			glGenBuffers(1, &immVBO);
			glBindVertexArray(immVAO);
			glBindBuffer(GL_ARRAY_BUFFER, immVBO);
			const GLsizei stride = sizeof(Vertex);
			glEnableVertexAttribArray(WebGLCompat::ATTRIB_VERTEX);
			glVertexAttribPointer(WebGLCompat::ATTRIB_VERTEX, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, pos)));
			glEnableVertexAttribArray(WebGLCompat::ATTRIB_COLOR);
			glVertexAttribPointer(WebGLCompat::ATTRIB_COLOR, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, color)));
			glEnableVertexAttribArray(WebGLCompat::ATTRIB_TEXCOORD0);
			glVertexAttribPointer(WebGLCompat::ATTRIB_TEXCOORD0, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, tex)));
			glEnableVertexAttribArray(WebGLCompat::ATTRIB_NORMAL);
			glVertexAttribPointer(WebGLCompat::ATTRIB_NORMAL, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, normal)));
		} else {
			glBindVertexArray(immVAO);
			glBindBuffer(GL_ARRAY_BUFFER, immVBO);
		}
		glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(Vertex), verts.data(), GL_STREAM_DRAW);

		const bool ffp = !WebGLCompat::HasCurrentProgram() && WebGLCompat::BeginFixedFunctionDraw(true);
		if (ffp || WebGLCompat::HasCurrentProgram()) {
			WebGLCompat::FlushFixedStateForDraw();
			WebGLCompat::RawDrawArrays(mode, 0, GLsizei(verts.size()));
		}
		if (WebGLCompat::traceDraws > 0) {
			WebGLCompat::traceDraws--;
			float mvp[16], tex[16];
			WebGLCompat::GetFixedMatrices(mvp, tex);
			const float* p = verts[0].pos;
			float c[4];
			for (int r = 0; r < 4; r++)
				c[r] = mvp[r] * p[0] + mvp[4 + r] * p[1] + mvp[8 + r] * p[2] + mvp[12 + r] * p[3];
			GLint prog = 0, fbo = 0, vp[4] = {0, 0, 0, 0};
			glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
			glGetIntegerv(GL_VIEWPORT, vp);
			LOG("[WebGLCompat::trace] imm mode=0x%x n=%d v0=(%.1f %.1f %.1f) clip=(%.2f %.2f %.2f %.2f) col=(%.2f %.2f %.2f %.2f) ffp=%d prog=%d fbo=%d vp=%d,%d,%d,%d depth=%d blend=%d tex2D=%d err=0x%x",
				mode, int(verts.size()), p[0], p[1], p[2], c[0], c[1], c[2], c[3], verts[0].color[0], verts[0].color[1], verts[0].color[2], verts[0].color[3],
				int(ffp), prog, fbo, vp[0], vp[1], vp[2], vp[3], int(glIsEnabled(GL_DEPTH_TEST)), int(glIsEnabled(GL_BLEND)), int(glIsEnabled(GL_TEXTURE_2D)), int(glGetError()));
		}
		if (ffp)
			WebGLCompat::EndFixedFunctionDraw();

		glBindVertexArray(GLuint(prevVAO));
		glBindBuffer(GL_ARRAY_BUFFER, GLuint(prevArrayBuf));
	}

	void APIENTRY compat_glBegin(GLenum mode) {
		batchMode = mode;
		inBeginEnd = true;
		batch.clear();
		batchInheritColor = 0;
		if (!listCompiling) {
			std::memcpy(immColor, WebGLCompat::GetCurrentColor(), sizeof(immColor));
			immColorKnown = true;
		}
	}

	void APIENTRY compat_glEnd() {
		if (!inBeginEnd)
			return;
		inBeginEnd = false;
		if (listCompiling) {
			ListRecord([mode = batchMode, verts = std::move(batch), inherit = batchInheritColor]() mutable { DrawBatch(mode, verts, inherit); });
			batch = {};
			return;
		}
		DrawBatch(batchMode, batch, 0);
	}

	void EmitVertex(float x, float y, float z, float w) {
		if (!inBeginEnd)
			return;
		Vertex v;
		v.pos[0] = x; v.pos[1] = y; v.pos[2] = z; v.pos[3] = w;
		std::memcpy(v.color, immColor, sizeof(v.color));
		std::memcpy(v.tex, immTex, sizeof(v.tex));
		std::memcpy(v.normal, immNormal, sizeof(v.normal));
		if (!immColorKnown && batchInheritColor == batch.size())
			batchInheritColor++;
		batch.push_back(v);
	}

	void APIENTRY compat_glVertex2f(GLfloat x, GLfloat y) { EmitVertex(x, y, 0.0f, 1.0f); }
	void APIENTRY compat_glVertex3f(GLfloat x, GLfloat y, GLfloat z) { EmitVertex(x, y, z, 1.0f); }
	void APIENTRY compat_glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { EmitVertex(x, y, z, w); }
	void APIENTRY compat_glVertex2fv(const GLfloat* v) { EmitVertex(v[0], v[1], 0.0f, 1.0f); }
	void APIENTRY compat_glVertex3fv(const GLfloat* v) { EmitVertex(v[0], v[1], v[2], 1.0f); }
	void APIENTRY compat_glVertex4fv(const GLfloat* v) { EmitVertex(v[0], v[1], v[2], v[3]); }
	void APIENTRY compat_glVertex2d(GLdouble x, GLdouble y) { EmitVertex(float(x), float(y), 0.0f, 1.0f); }
	void APIENTRY compat_glVertex3d(GLdouble x, GLdouble y, GLdouble z) { EmitVertex(float(x), float(y), float(z), 1.0f); }
	void APIENTRY compat_glVertex2i(GLint x, GLint y) { EmitVertex(float(x), float(y), 0.0f, 1.0f); }
	void APIENTRY compat_glVertex3i(GLint x, GLint y, GLint z) { EmitVertex(float(x), float(y), float(z), 1.0f); }

	void SetTex(float s, float t, float r, float q) { immTex[0] = s; immTex[1] = t; immTex[2] = r; immTex[3] = q; }
	void APIENTRY compat_glTexCoord1f(GLfloat s) { SetTex(s, 0.0f, 0.0f, 1.0f); }
	void APIENTRY compat_glTexCoord2f(GLfloat s, GLfloat t) { SetTex(s, t, 0.0f, 1.0f); }
	void APIENTRY compat_glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) { SetTex(s, t, r, 1.0f); }
	void APIENTRY compat_glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { SetTex(s, t, r, q); }
	void APIENTRY compat_glTexCoord2fv(const GLfloat* v) { SetTex(v[0], v[1], 0.0f, 1.0f); }
	// only unit 0 has a slot in the immediate vertex format
	void APIENTRY compat_glMultiTexCoord1f(GLenum unit, GLfloat s) { if (unit == GL_TEXTURE0) SetTex(s, 0.0f, 0.0f, 1.0f); }
	void APIENTRY compat_glMultiTexCoord2f(GLenum unit, GLfloat s, GLfloat t) { if (unit == GL_TEXTURE0) SetTex(s, t, 0.0f, 1.0f); }
	void APIENTRY compat_glMultiTexCoord3f(GLenum unit, GLfloat s, GLfloat t, GLfloat r) { if (unit == GL_TEXTURE0) SetTex(s, t, r, 1.0f); }
	void APIENTRY compat_glMultiTexCoord4f(GLenum unit, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { if (unit == GL_TEXTURE0) SetTex(s, t, r, q); }
	void APIENTRY compat_glNormal3f(GLfloat x, GLfloat y, GLfloat z) { immNormal[0] = x; immNormal[1] = y; immNormal[2] = z; }
	void APIENTRY compat_glNormal3fv(const GLfloat* v) { compat_glNormal3f(v[0], v[1], v[2]); }

	// glColor: WebGLState keeps the current color (and records the call in a list); the
	// immediate-mode color has to follow right away, also while a list is compiled
	REAL(glColor4f); REAL(glColor3f); REAL(glColor4fv); REAL(glColor3fv); REAL(glColor4ub); REAL(glColor4ubv);
	void ImmColor(float r, float g, float b, float a) { immColor[0] = r; immColor[1] = g; immColor[2] = b; immColor[3] = a; immColorKnown = true; }
	void APIENTRY compat_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { ImmColor(r, g, b, a); real_glColor4f(r, g, b, a); }
	void APIENTRY compat_glColor3f(GLfloat r, GLfloat g, GLfloat b) { ImmColor(r, g, b, 1.0f); real_glColor3f(r, g, b); }
	void APIENTRY compat_glColor4fv(const GLfloat* v) { ImmColor(v[0], v[1], v[2], v[3]); real_glColor4fv(v); }
	void APIENTRY compat_glColor3fv(const GLfloat* v) { ImmColor(v[0], v[1], v[2], 1.0f); real_glColor3fv(v); }
	void APIENTRY compat_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { ImmColor(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); real_glColor4ub(r, g, b, a); }
	void APIENTRY compat_glColor4ubv(const GLubyte* v) { ImmColor(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f); real_glColor4ubv(v); }

	void APIENTRY compat_glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) {
		compat_glBegin(GL_QUADS);
		EmitVertex(x1, y1, 0.0f, 1.0f);
		EmitVertex(x2, y1, 0.0f, 1.0f);
		EmitVertex(x2, y2, 0.0f, 1.0f);
		EmitVertex(x1, y2, 0.0f, 1.0f);
		compat_glEnd();
	}

	// ---- fixed-function parameters ----------------------------------------------------------
	void APIENTRY compat_glAlphaFunc(GLenum func, GLfloat ref) {
		if (listCompiling) { ListRecord([=] { compat_glAlphaFunc(func, ref); }); return; }
		alphaFunc = func;
		alphaRef = ref;
	}
	void APIENTRY compat_glPointSize(GLfloat size) {
		if (listCompiling) { ListRecord([=] { compat_glPointSize(size); }); return; }
		pointSize = size;
	}
	// accepted and ignored: flat shading, lighting model and materials (the engine's lit
	// paths are all shaders), texture environment (modulate is assumed), point parameters,
	// logic ops, line stipple and wireframe polygon mode (not available in WebGL2)
	void APIENTRY compat_glShadeModel(GLenum) {}
	void APIENTRY compat_glLightModeli(GLenum, GLint) {}
	void APIENTRY compat_glLightModelfv(GLenum, const GLfloat*) {}
	void APIENTRY compat_glMaterialf(GLenum, GLenum, GLfloat) {}
	void APIENTRY compat_glMaterialfv(GLenum, GLenum, const GLfloat*) {}
	void APIENTRY compat_glTexEnvi(GLenum, GLenum, GLint) {}
	void APIENTRY compat_glTexEnvf(GLenum, GLenum, GLfloat) {}
	void APIENTRY compat_glTexEnvfv(GLenum, GLenum, const GLfloat*) {}
	void APIENTRY compat_glPointParameterf(GLenum, GLfloat) {}
	void APIENTRY compat_glPointParameterfv(GLenum, const GLfloat*) {}
	void APIENTRY compat_glLogicOp(GLenum) {}
	void APIENTRY compat_glLineStipple(GLint, GLushort) {}

	// ---- display lists ----------------------------------------------------------------------
	struct DisplayList { std::vector<std::function<void()>> cmds; };
	std::unordered_map<GLuint, DisplayList> lists;
	GLuint nextListId = 1;
	GLuint compilingId = 0;
	GLenum compilingMode = GL_COMPILE;
	int callDepth = 0;

	GLuint APIENTRY compat_glGenLists(GLsizei range) {
		if (range <= 0)
			return 0;
		const GLuint base = nextListId;
		nextListId += range;
		for (GLsizei i = 0; i < range; i++)
			lists[base + i] = {};
		return base;
	}

	void APIENTRY compat_glNewList(GLuint list, GLenum mode) {
		if (listCompiling || list == 0)
			return;
		lists[list].cmds.clear();
		compilingId = list;
		compilingMode = mode;
		listCompiling = true;
		immColorKnown = false;
	}

	void RunList(GLuint list) {
		const auto it = lists.find(list);
		if (it == lists.end() || callDepth >= 64)
			return;
		callDepth++;
		// (by index: a command may call glNewList/glDeleteLists and rehash the map; lists
		// modifying themselves while running are not supported)
		for (size_t i = 0; i < it->second.cmds.size(); i++)
			it->second.cmds[i]();
		callDepth--;
	}

	void APIENTRY compat_glEndList() {
		if (!listCompiling)
			return;
		listCompiling = false;
		immColorKnown = true;
		const GLuint id = compilingId;
		compilingId = 0;
		if (compilingMode == GL_COMPILE_AND_EXECUTE)
			RunList(id);
	}

	void APIENTRY compat_glCallList(GLuint list) {
		if (listCompiling) { ListRecord([=] { compat_glCallList(list); }); return; }
		RunList(list);
	}

	void APIENTRY compat_glDeleteLists(GLuint list, GLsizei range) {
		for (GLsizei i = 0; i < range; i++)
			lists.erase(list + i);
	}

	GLboolean APIENTRY compat_glIsList(GLuint list) { return lists.count(list) != 0; }

	// native state calls that commonly appear inside display lists
	#define CAPTURE_HOOK(name, params, args) \
		decltype(glad_##name) real_##name = nullptr; \
		void APIENTRY compat_##name params { \
			if (listCompiling) { ListRecord([=] { real_##name args; }); return; } \
			real_##name args; \
		}
	CAPTURE_HOOK(glBlendFunc, (GLenum s, GLenum d), (s, d))
	CAPTURE_HOOK(glBlendFuncSeparate, (GLenum sc, GLenum dc, GLenum sa, GLenum da), (sc, dc, sa, da))
	CAPTURE_HOOK(glBlendEquation, (GLenum m), (m))
	CAPTURE_HOOK(glDepthMask, (GLboolean f), (f))
	CAPTURE_HOOK(glDepthFunc, (GLenum f), (f))
	CAPTURE_HOOK(glLineWidth, (GLfloat w), (w))
	CAPTURE_HOOK(glCullFace, (GLenum m), (m))
	CAPTURE_HOOK(glPolygonOffset, (GLfloat f, GLfloat u), (f, u))
	CAPTURE_HOOK(glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a), (r, g, b, a))
	CAPTURE_HOOK(glScissor, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h))
	CAPTURE_HOOK(glStencilFunc, (GLenum f, GLint r, GLuint m), (f, r, m))
	CAPTURE_HOOK(glStencilOp, (GLenum a, GLenum b, GLenum c), (a, b, c))
	CAPTURE_HOOK(glStencilMask, (GLuint m), (m))
	CAPTURE_HOOK(glUniform1i, (GLint l, GLint a), (l, a))
	CAPTURE_HOOK(glUniform1f, (GLint l, GLfloat a), (l, a))
	CAPTURE_HOOK(glUniform2f, (GLint l, GLfloat a, GLfloat b), (l, a, b))
	CAPTURE_HOOK(glUniform3f, (GLint l, GLfloat a, GLfloat b, GLfloat c), (l, a, b, c))
	CAPTURE_HOOK(glUniform4f, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d), (l, a, b, c, d))
	CAPTURE_HOOK(glUniform2i, (GLint l, GLint a, GLint b), (l, a, b))
	CAPTURE_HOOK(glUniform3i, (GLint l, GLint a, GLint b, GLint c), (l, a, b, c))
	CAPTURE_HOOK(glUniform4i, (GLint l, GLint a, GLint b, GLint c, GLint d), (l, a, b, c, d))

	// array uniforms: the data is copied at record time
	#define CAPTURE_VEC_HOOK(name, T, n) \
		decltype(glad_##name) real_##name = nullptr; \
		void APIENTRY compat_##name(GLint l, GLsizei count, const T* v) { \
			if (listCompiling) { \
				std::vector<T> d(v, v + size_t(std::max(count, 0)) * (n)); \
				ListRecord([=, d = std::move(d)] { real_##name(l, count, d.data()); }); \
				return; \
			} \
			real_##name(l, count, v); \
		}
	#define CAPTURE_MAT_HOOK(name, n) \
		decltype(glad_##name) real_##name = nullptr; \
		void APIENTRY compat_##name(GLint l, GLsizei count, GLboolean transpose, const GLfloat* v) { \
			if (listCompiling) { \
				std::vector<GLfloat> d(v, v + size_t(std::max(count, 0)) * (n)); \
				ListRecord([=, d = std::move(d)] { real_##name(l, count, transpose, d.data()); }); \
				return; \
			} \
			real_##name(l, count, transpose, v); \
		}
	CAPTURE_VEC_HOOK(glUniform1fv, GLfloat, 1) CAPTURE_VEC_HOOK(glUniform2fv, GLfloat, 2)
	CAPTURE_VEC_HOOK(glUniform3fv, GLfloat, 3) CAPTURE_VEC_HOOK(glUniform4fv, GLfloat, 4)
	CAPTURE_VEC_HOOK(glUniform1iv, GLint, 1) CAPTURE_VEC_HOOK(glUniform2iv, GLint, 2)
	CAPTURE_VEC_HOOK(glUniform3iv, GLint, 3) CAPTURE_VEC_HOOK(glUniform4iv, GLint, 4)
	CAPTURE_MAT_HOOK(glUniformMatrix3fv, 9) CAPTURE_MAT_HOOK(glUniformMatrix4fv, 16)

	// ---- draw calls inside display lists ------------------------------------------------------
	// Desktop GL copies the vertex data a draw reads into the list at compile time (the
	// engine's font rendering in Lua lists reuses its buffers right after). The snapshot
	// keeps that data in its own VAO; program, uniforms, constant attributes and
	// fixed-function state are taken from when the list runs, as on desktop.
	struct DrawSnapshot {
		GLuint vao = 0, vbo = 0, ebo = 0;
		GLenum mode = GL_TRIANGLES;
		GLsizei count = 0;
		GLsizei instances = 0;
		bool indexed = false;

		DrawSnapshot() = default;
		DrawSnapshot(const DrawSnapshot&) = delete;
		~DrawSnapshot() {
			// (lists can outlive the context at exit)
			if (emscripten_webgl_get_current_context() == 0)
				return;
			if (vao != 0) glDeleteVertexArrays(1, &vao);
			if (vbo != 0) glDeleteBuffers(1, &vbo);
			if (ebo != 0) glDeleteBuffers(1, &ebo);
		}
	};

	size_t AttribElemBytes(const WebGLCompat::AttribShadow& a) {
		switch (a.type) {
			case GL_BYTE: case GL_UNSIGNED_BYTE: return size_t(a.size);
			case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return size_t(a.size) * 2;
			case GL_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
			default: return size_t(a.size) * 4;
		}
	}

	uint32_t listDrawsRecorded = 0;
	uint32_t listDrawsFailed = 0;

	void RunDrawSnapshot(const DrawSnapshot& snap) {
		GLint prevVAO = 0;
		GLint prevArrayBuf = 0;
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
		glBindVertexArray(snap.vao);

		const bool ffp = !WebGLCompat::HasCurrentProgram() && WebGLCompat::BeginFixedFunctionDraw(false);
		if (ffp || WebGLCompat::HasCurrentProgram()) {
			WebGLCompat::FlushFixedStateForDraw();
			WebGLCompat::RawDraw(snap.mode, 0, snap.count, snap.indexed, snap.instances);
		}
		if (ffp)
			WebGLCompat::EndFixedFunctionDraw();

		glBindVertexArray(GLuint(prevVAO));
		glBindBuffer(GL_ARRAY_BUFFER, GLuint(prevArrayBuf));
	}
}


void WebGLCompat::ListRecord(std::function<void()>&& cmd)
{
	if (const auto it = lists.find(compilingId); it != lists.end())
		it->second.cmds.push_back(std::move(cmd));
}

GLuint WebGLCompat::GetCompilingList() { return listCompiling? compilingId: 0; }
GLenum WebGLCompat::GetCompilingListMode() { return listCompiling? compilingMode: 0; }

void WebGLCompat::ListRecordDraw(GLenum mode, GLint first, GLsizei count, GLenum indexType, const void* indices, GLsizei instances)
{
	if (count <= 0)
		return;

	// ---- indices: read, rebase to the lowest vertex, GL_QUADS -> triangles
	std::vector<GLuint> idx;
	GLuint minV = 0;
	GLuint maxV = 0;
	if (indexType != 0) {
		const size_t isz = (indexType == GL_UNSIGNED_BYTE)? 1: ((indexType == GL_UNSIGNED_SHORT)? 2: 4);
		std::vector<uint8_t> raw(size_t(count) * isz);

		GLint elemBuf = 0;
		glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elemBuf);
		if (elemBuf != 0) {
			glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, GLintptr(reinterpret_cast<uintptr_t>(indices)), GLsizeiptr(raw.size()), raw.data());
		} else if (indices != nullptr) {
			std::memcpy(raw.data(), indices, raw.size());
		} else {
			listDrawsFailed++;
			return;
		}

		std::vector<GLuint> src(count);
		for (GLsizei i = 0; i < count; i++) {
			switch (isz) {
				case 1: src[i] = raw[i]; break;
				case 2: { uint16_t v; std::memcpy(&v, &raw[i * 2], 2); src[i] = v; } break;
				default: { uint32_t v; std::memcpy(&v, &raw[i * 4], 4); src[i] = v; } break;
			}
		}
		minV = *std::min_element(src.begin(), src.end());
		maxV = *std::max_element(src.begin(), src.end());

		if (mode == GL_QUADS) {
			idx.reserve(size_t(count / 4) * 6);
			for (GLsizei q = 0; q + 3 < count; q += 4) {
				const GLuint* v = &src[q];
				for (const int k: {0, 1, 2, 0, 2, 3})
					idx.push_back(v[k] - minV);
			}
			mode = GL_TRIANGLES;
		} else {
			idx.resize(src.size());
			for (size_t i = 0; i < src.size(); i++)
				idx[i] = src[i] - minV;
		}
		if (idx.empty())
			return;
	} else {
		minV = GLuint(std::max(first, 0));
		maxV = minV + GLuint(count) - 1;
	}

	// ---- vertex data of every enabled attribute (read before binding the new VAO)
	struct Source {
		GLuint index;
		AttribShadow a;
		size_t stride;
		uintptr_t start;
		size_t bytes;
		size_t dstOff;
	};
	std::vector<Source> sources;
	size_t total = 0;
	for (GLuint i = 0; i < MAX_SHADOW_ATTRIBS; i++) {
		const AttribShadow* a = GetAttribShadow(i);
		if (a == nullptr || !a->enabled)
			continue;
		const size_t elem = AttribElemBytes(*a);
		const size_t stride = (a->stride != 0)? size_t(a->stride): elem;
		size_t v0 = minV;
		size_t v1 = maxV;
		if (a->divisor != 0) {
			v0 = 0;
			v1 = (instances > 1)? size_t(instances - 1) / a->divisor: 0;
		}
		Source s{i, *a, stride, a->ptr + v0 * stride, (v1 - v0) * stride + elem, 0};
		total = (total + 15) & ~size_t(15);
		s.dstOff = total;
		total += s.bytes;
		sources.push_back(s);
	}

	GLint prevVAO = 0, prevArrayBuf = 0, prevCopyRead = 0;
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
	glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &prevCopyRead);

	auto snap = std::make_shared<DrawSnapshot>();
	snap->mode = mode;
	snap->count = (indexType != 0)? GLsizei(idx.size()): count;
	snap->instances = instances;
	snap->indexed = (indexType != 0);

	glGenVertexArrays(1, &snap->vao);
	glBindVertexArray(snap->vao);
	if (total > 0) {
		glGenBuffers(1, &snap->vbo);
		glBindBuffer(GL_ARRAY_BUFFER, snap->vbo);
		glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(total), nullptr, GL_STATIC_DRAW);
	}
	for (const Source& s: sources) {
		if (s.a.buffer == 0) {
			if (s.start != 0)
				glBufferSubData(GL_ARRAY_BUFFER, GLintptr(s.dstOff), GLsizeiptr(s.bytes), reinterpret_cast<const void*>(s.start));
			else
				listDrawsFailed++;
		} else {
			glBindBuffer(GL_COPY_READ_BUFFER, s.a.buffer);
			GLint srcSize = 0;
			glGetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &srcSize);
			const size_t avail = (s.start < size_t(srcSize))? size_t(srcSize) - s.start: 0;
			const size_t n = std::min(s.bytes, avail);
			if (n > 0)
				glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_ARRAY_BUFFER, GLintptr(s.start), GLintptr(s.dstOff), GLsizeiptr(n));
			else
				listDrawsFailed++;
		}
		const void* off = reinterpret_cast<const void*>(s.dstOff);
		glEnableVertexAttribArray(s.index);
		if (s.a.integer)
			glVertexAttribIPointer(s.index, s.a.size, s.a.type, GLsizei(s.stride), off);
		else
			glVertexAttribPointer(s.index, s.a.size, s.a.type, s.a.normalized? GL_TRUE: GL_FALSE, GLsizei(s.stride), off);
		if (s.a.divisor != 0)
			glVertexAttribDivisor(s.index, s.a.divisor);
	}
	if (snap->indexed) {
		glGenBuffers(1, &snap->ebo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, snap->ebo); // (part of the snapshot VAO)
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * sizeof(GLuint)), idx.data(), GL_STATIC_DRAW);
	}

	glBindVertexArray(GLuint(prevVAO));
	glBindBuffer(GL_ARRAY_BUFFER, GLuint(prevArrayBuf));
	glBindBuffer(GL_COPY_READ_BUFFER, GLuint(prevCopyRead));

	listDrawsRecorded++;
	ListRecord([snap] { RunDrawSnapshot(*snap); });
}

uint32_t WebGLCompat::GetListDrawStats(uint32_t* failed)
{
	if (failed != nullptr)
		*failed = listDrawsFailed;
	return listDrawsRecorded;
}


bool WebGLCompat::BeginFixedFunctionDraw(bool immediate)
{
	if (!CreateFixedFunctionProgram())
		return false;

	glUseProgram(ffpProgram);
	glUniform1i(ffpLocTexMode, IsLocalCapEnabled(GL_TEXTURE_2D)? 1: 0);
	glUniform1i(ffpLocAlphaMode, AlphaMode());
	glUniform1f(ffpLocAlphaRef, alphaRef);
	glUniform1f(ffpLocPointSize, pointSize);

	if (!immediate) {
		// client arrays: attributes without an enabled array take these constants
		glVertexAttrib4fv(ATTRIB_COLOR, GetCurrentColor());
		glVertexAttrib4f(ATTRIB_TEXCOORD0, 0.0f, 0.0f, 0.0f, 1.0f);
	}
	return true;
}

void WebGLCompat::EndFixedFunctionDraw()
{
	glUseProgram(0);
}


void WebGLCompat::InstallImmediateEmulation()
{
	#define EMULATE(name) glad_##name = compat_##name
	EMULATE(glBegin); EMULATE(glEnd);
	EMULATE(glVertex2f); EMULATE(glVertex3f); EMULATE(glVertex4f);
	EMULATE(glVertex2fv); EMULATE(glVertex3fv); EMULATE(glVertex4fv);
	EMULATE(glVertex2d); EMULATE(glVertex3d); EMULATE(glVertex2i); EMULATE(glVertex3i);
	EMULATE(glTexCoord1f); EMULATE(glTexCoord2f); EMULATE(glTexCoord3f); EMULATE(glTexCoord4f); EMULATE(glTexCoord2fv);
	EMULATE(glMultiTexCoord1f); EMULATE(glMultiTexCoord2f); EMULATE(glMultiTexCoord3f); EMULATE(glMultiTexCoord4f);
	EMULATE(glNormal3f); EMULATE(glNormal3fv);
	EMULATE(glRectf);
	EMULATE(glAlphaFunc); EMULATE(glPointSize);
	EMULATE(glShadeModel); EMULATE(glLightModeli); EMULATE(glLightModelfv);
	EMULATE(glMaterialf); EMULATE(glMaterialfv);
	EMULATE(glTexEnvi); EMULATE(glTexEnvf); EMULATE(glTexEnvfv);
	EMULATE(glPointParameterf); EMULATE(glPointParameterfv);
	EMULATE(glLogicOp); EMULATE(glLineStipple);
	EMULATE(glGenLists); EMULATE(glNewList); EMULATE(glEndList); EMULATE(glCallList); EMULATE(glDeleteLists); EMULATE(glIsList);
	#undef EMULATE

	HOOK(glColor4f); HOOK(glColor3f); HOOK(glColor4fv); HOOK(glColor3fv); HOOK(glColor4ub); HOOK(glColor4ubv);

	HOOK(glBlendFunc); HOOK(glBlendFuncSeparate); HOOK(glBlendEquation);
	HOOK(glDepthMask); HOOK(glDepthFunc); HOOK(glLineWidth); HOOK(glCullFace); HOOK(glPolygonOffset);
	HOOK(glColorMask); HOOK(glScissor); HOOK(glStencilFunc); HOOK(glStencilOp); HOOK(glStencilMask);
	HOOK(glUniform1i); HOOK(glUniform1f); HOOK(glUniform2f); HOOK(glUniform3f); HOOK(glUniform4f);
	HOOK(glUniform2i); HOOK(glUniform3i); HOOK(glUniform4i);
	HOOK(glUniform1fv); HOOK(glUniform2fv); HOOK(glUniform3fv); HOOK(glUniform4fv);
	HOOK(glUniform1iv); HOOK(glUniform2iv); HOOK(glUniform3iv); HOOK(glUniform4iv);
	HOOK(glUniformMatrix3fv); HOOK(glUniformMatrix4fv);
}
