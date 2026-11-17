/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <glad/glad.h>

// shared between the WebGLCompat source files only
namespace WebGLCompat {
	// WebGLState.cpp: replace glad pointers with the fixed-function emulation;
	// called by Load() once every pointer is non-null
	void InstallStateEmulation();

	// WebGLObjects.cpp: buffer mapping, texture bookkeeping and format mapping;
	// called after InstallStateEmulation()
	void InstallObjectEmulation();

	// WebGLImmediate.cpp: glBegin/glEnd, display lists and the fixed-function program;
	// called after InstallObjectEmulation()
	void InstallImmediateEmulation();

	// WebGLStorage.cpp: read-only shader storage buffers as textures; called last
	void InstallStorageEmulation();

	// WebGLDrawBase.cpp: base-vertex/base-instance and client-memory indirect draws; called
	// after InstallStorageEmulation(), so its draws go through every other hook
	void InstallDrawBaseEmulation();
	struct StorageUse {
		int binding;
		GLint unit;     // texture unit of its mirror in this program
		GLint bytesLoc; // rcl_ssboN_bytes, for member.length()
	};
	// the texture unit for a program's slot-th storage binding (-1: too many)
	GLint StorageTextureUnit(int slot);
	// before a draw: refresh the textures of the bindings the program reads and bind them
	void BindStorageForDraw(const std::vector<StorageUse>& uses);

	// debugging aid for memory corruption (RECOIL_WEB_CANARY=1): checks that the start of
	// static data is intact and logs (once) the first checkpoint at which it is not
	void Canary(const char* where);

	// draws dropped because their primitive mode has no WebGL2 equivalent yet
	uint32_t GetSkippedDrawCount();

	// ---- fixed-function state (WebGLState.cpp) used by the immediate-mode emulation
	const float* GetCurrentColor();
	bool HasCurrentProgram();
	GLuint GetCurrentProgram();
	bool IsLocalCapEnabled(GLenum cap);       // caps WebGL2 lacks (GL_TEXTURE_2D, GL_ALPHA_TEST, ...)
	void FlushFixedStateForDraw();            // upload rcl_* uniforms of the bound program
	void GetFixedMatrices(float* mvp, float* tex0);
	void RawDrawArrays(GLenum mode, GLint first, GLsizei count); // maps GL_QUADS etc., no state flush
	void RawDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
	void RawUseProgram(GLuint program);       // bypasses program tracking
	// draw without state flush; see the definition for the indexed form
	void RawDraw(GLenum mode, GLint first, GLsizei count, bool indexed, GLsizei instances);

	// shadow of one vertex attribute of the bound VAO (nullptr for indices past the shadow)
	constexpr uint32_t MAX_SHADOW_ATTRIBS = 16;
	struct AttribShadow {
		bool enabled = false;
		bool normalized = false;
		bool integer = false;   // glVertexAttribIPointer
		GLint size = 4;
		GLenum type = GL_FLOAT;
		GLsizei stride = 0;
		GLuint divisor = 0;
		GLuint buffer = 0;      // GL_ARRAY_BUFFER at pointer time; 0: client pointer
		uintptr_t ptr = 0;      // buffer offset or client address
	};
	const AttribShadow* GetAttribShadow(GLuint index);

	// ---- fixed-function program (WebGLImmediate.cpp): bound around draws issued while no
	// shader is in use. immediate: vertex data comes from the immediate-mode buffer;
	// otherwise from the client arrays, with constant color/texcoord attributes.
	bool BeginFixedFunctionDraw(bool immediate);
	void EndFixedFunctionDraw();

	// ---- display lists (WebGLImmediate.cpp): while a list is being compiled, emulated
	// entry points record themselves as closures instead of executing
	extern bool listCompiling;
	// debugging aid: number of immediate-mode draws still to log (set at each frame capture)
	extern int traceDraws;
	GLuint GetCompilingList();     // 0 if none
	GLenum GetCompilingListMode();
	void ListRecord(std::function<void()>&& cmd);
	// a draw call inside a list: like desktop GL, the vertex (and index) data the draw
	// would read is copied out of the bound arrays now and replayed when the list runs.
	// indexType 0: glDrawArrays(first, count); else glDrawElements(count, indexType, indices)
	void ListRecordDraw(GLenum mode, GLint first, GLsizei count, GLenum indexType, const void* indices, GLsizei instances);
	uint32_t GetListDrawStats(uint32_t* failed); // draws recorded into lists so far
}
