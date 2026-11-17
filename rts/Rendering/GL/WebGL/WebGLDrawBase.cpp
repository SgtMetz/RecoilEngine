/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Base-vertex, base-instance and indirect draws for WebGL2, which has none of them
// (WEBGL_draw_instanced_base_vertex_base_instance is a draft extension, off by default).
//
// - base vertex: every enabled per-vertex attribute (divisor 0) of the bound VAO is
//   re-pointed baseVertex elements further, the draw is issued, and the pointers are
//   restored. Indices stay relative, which is what base-vertex semantics are.
// - base instance: the same for instanced attributes (divisor d > 0), by baseInstance / d
//   elements. gl_InstanceID stays relative to the base, as in desktop GL; no shader the
//   engine or BAR ships reads gl_BaseInstance.
// - glMultiDraw{Arrays,Elements}Indirect with a client-memory command array (the engine
//   passes one; WebGL2 has no GL_DRAW_INDIRECT_BUFFER target): one draw per command.
//
// Draws go through the glad pointers as they are when this is installed (last), so the
// storage-buffer binding, fixed-state flush and polygon-mode emulation all still apply.

#include "WebGLCompat.h"
#include "WebGLInternal.h"

#include <glad/glad.h>

#include <cstdint>
#include <cstdlib>

#include "System/Log/ILog.h"

namespace {
	decltype(glad_glDrawElements) next_glDrawElements = nullptr;
	decltype(glad_glDrawElementsInstanced) next_glDrawElementsInstanced = nullptr;
	decltype(glad_glDrawArraysInstanced) next_glDrawArraysInstanced = nullptr;
	decltype(glad_glVertexAttribPointer) next_glVertexAttribPointer = nullptr;
	decltype(glad_glVertexAttribIPointer) next_glVertexAttribIPointer = nullptr;
	decltype(glad_glBindBuffer) next_glBindBuffer = nullptr;
	decltype(glad_glGetIntegerv) next_glGetIntegerv = nullptr;

	uint32_t numShifted = 0;
	uint32_t numIndirect = 0;
	uint32_t numInexact = 0;

	GLsizei TypeSize(GLenum type) {
		switch (type) {
			case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
			case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2;
			case GL_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
			default: return 4;
		}
	}
	GLsizei AttribStride(const WebGLCompat::AttribShadow& s) {
		if (s.stride != 0)
			return s.stride;
		const GLenum t = s.type;
		if (t == GL_INT_2_10_10_10_REV || t == GL_UNSIGNED_INT_2_10_10_10_REV)
			return 4;
		return s.size * TypeSize(t);
	}

	void Point(GLuint i, const WebGLCompat::AttribShadow& s, uintptr_t ptr) {
		next_glBindBuffer(GL_ARRAY_BUFFER, s.buffer);
		if (s.integer)
			next_glVertexAttribIPointer(i, s.size, s.type, s.stride, reinterpret_cast<const void*>(ptr));
		else
			next_glVertexAttribPointer(i, s.size, s.type, s.normalized? GL_TRUE: GL_FALSE, s.stride, reinterpret_cast<const void*>(ptr));
	}

	// shift the bound VAO's attributes; returns a bitmask of shifted attributes, with
	// copies of their shadows in saved[] for Restore
	uint32_t Shift(GLint baseVertex, GLuint baseInstance, WebGLCompat::AttribShadow* saved) {
		if (baseVertex == 0 && baseInstance == 0)
			return 0;
		uint32_t mask = 0;
		GLint arrayBuf = 0;
		for (GLuint i = 0; i < WebGLCompat::MAX_SHADOW_ATTRIBS; i++) {
			const WebGLCompat::AttribShadow* s = WebGLCompat::GetAttribShadow(i);
			if (s == nullptr || !s->enabled)
				continue;
			int64_t elems = 0;
			if (s->divisor == 0) {
				elems = baseVertex;
			} else {
				elems = baseInstance / s->divisor;
				if ((baseInstance % s->divisor) != 0 && numInexact++ == 0)
					LOG_L(L_WARNING, "[WebGLCompat] base instance %u is not a multiple of attribute %u's divisor %u", baseInstance, i, s->divisor);
			}
			if (elems == 0)
				continue;
			if (mask == 0)
				next_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
			saved[i] = *s;
			mask |= (1u << i);
			Point(i, saved[i], uintptr_t(int64_t(saved[i].ptr) + elems * AttribStride(saved[i])));
		}
		if (mask != 0) {
			next_glBindBuffer(GL_ARRAY_BUFFER, GLuint(arrayBuf));
			if (numShifted++ == 0)
				LOG("[WebGLCompat] first base-vertex/base-instance draw emulated by attribute offsets (mask 0x%x)", mask);
		}
		return mask;
	}
	void Restore(uint32_t mask, const WebGLCompat::AttribShadow* saved) {
		if (mask == 0)
			return;
		GLint arrayBuf = 0;
		next_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
		for (GLuint i = 0; i < WebGLCompat::MAX_SHADOW_ATTRIBS; i++) {
			if ((mask & (1u << i)) != 0)
				Point(i, saved[i], saved[i].ptr);
		}
		next_glBindBuffer(GL_ARRAY_BUFFER, GLuint(arrayBuf));
	}

	void DrawElementsBase(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint baseVertex, GLuint baseInstance, bool instanced) {
		WebGLCompat::AttribShadow saved[WebGLCompat::MAX_SHADOW_ATTRIBS];
		const uint32_t mask = Shift(baseVertex, baseInstance, saved);
		if (instanced)
			next_glDrawElementsInstanced(mode, count, type, indices, instances);
		else
			next_glDrawElements(mode, count, type, indices);
		Restore(mask, saved);
	}

	void APIENTRY compat_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint baseVertex) {
		DrawElementsBase(mode, count, type, indices, 0, baseVertex, 0, false);
	}
	void APIENTRY compat_glDrawRangeElementsBaseVertex(GLenum mode, GLuint, GLuint, GLsizei count, GLenum type, const void* indices, GLint baseVertex) {
		DrawElementsBase(mode, count, type, indices, 0, baseVertex, 0, false);
	}
	void APIENTRY compat_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint baseVertex) {
		DrawElementsBase(mode, count, type, indices, instances, baseVertex, 0, true);
	}
	void APIENTRY compat_glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLuint baseInstance) {
		DrawElementsBase(mode, count, type, indices, instances, 0, baseInstance, true);
	}
	void APIENTRY compat_glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint baseVertex, GLuint baseInstance) {
		DrawElementsBase(mode, count, type, indices, instances, baseVertex, baseInstance, true);
	}
	void APIENTRY compat_glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instances, GLuint baseInstance) {
		WebGLCompat::AttribShadow saved[WebGLCompat::MAX_SHADOW_ATTRIBS];
		const uint32_t mask = Shift(0, baseInstance, saved);
		next_glDrawArraysInstanced(mode, first, count, instances);
		Restore(mask, saved);
	}

	void APIENTRY compat_glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawCount, GLsizei stride) {
		if (indirect == nullptr)
			return;
		if (stride == 0)
			stride = 5 * sizeof(GLuint);
		const GLsizei typeSize = TypeSize(type);
		if (numIndirect++ == 0)
			LOG("[WebGLCompat] first glMultiDrawElementsIndirect emulated (%d commands)", int(drawCount));
		for (GLsizei d = 0; d < drawCount; d++) {
			const GLuint* c = reinterpret_cast<const GLuint*>(static_cast<const char*>(indirect) + size_t(d) * stride);
			// count, instanceCount, firstIndex, baseVertex, baseInstance
			if (c[0] == 0 || c[1] == 0)
				continue;
			DrawElementsBase(mode, GLsizei(c[0]), type, reinterpret_cast<const void*>(uintptr_t(c[2]) * typeSize), GLsizei(c[1]), GLint(c[3]), c[4], true);
		}
	}
	void APIENTRY compat_glDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
		compat_glMultiDrawElementsIndirect(mode, type, indirect, 1, 0);
	}
	void APIENTRY compat_glMultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawCount, GLsizei stride) {
		if (indirect == nullptr)
			return;
		if (stride == 0)
			stride = 4 * sizeof(GLuint);
		numIndirect++;
		for (GLsizei d = 0; d < drawCount; d++) {
			const GLuint* c = reinterpret_cast<const GLuint*>(static_cast<const char*>(indirect) + size_t(d) * stride);
			// count, instanceCount, first, baseInstance
			if (c[0] == 0 || c[1] == 0)
				continue;
			compat_glDrawArraysInstancedBaseInstance(mode, GLint(c[2]), GLsizei(c[0]), GLsizei(c[1]), c[3]);
		}
	}
	void APIENTRY compat_glDrawArraysIndirect(GLenum mode, const void* indirect) {
		compat_glMultiDrawArraysIndirect(mode, indirect, 1, 0);
	}
}

void WebGLCompat::InstallDrawBaseEmulation()
{
	next_glDrawElements = glad_glDrawElements;
	next_glDrawElementsInstanced = glad_glDrawElementsInstanced;
	next_glDrawArraysInstanced = glad_glDrawArraysInstanced;
	next_glVertexAttribPointer = glad_glVertexAttribPointer;
	next_glVertexAttribIPointer = glad_glVertexAttribIPointer;
	next_glBindBuffer = glad_glBindBuffer;
	next_glGetIntegerv = glad_glGetIntegerv;

	#define EMULATE(name) glad_##name = compat_##name
	EMULATE(glDrawElementsBaseVertex);
	EMULATE(glDrawRangeElementsBaseVertex);
	EMULATE(glDrawElementsInstancedBaseVertex);
	EMULATE(glDrawElementsInstancedBaseInstance);
	EMULATE(glDrawElementsInstancedBaseVertexBaseInstance);
	EMULATE(glDrawArraysInstancedBaseInstance);
	EMULATE(glMultiDrawElementsIndirect);
	EMULATE(glDrawElementsIndirect);
	EMULATE(glMultiDrawArraysIndirect);
	EMULATE(glDrawArraysIndirect);
	#undef EMULATE
}
