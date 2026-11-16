/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Buffer and texture object emulation for the WebGL2 shim (see WebGLCompat.h):
//  - glMapBuffer / glMapBufferRange / glUnmapBuffer through CPU-side copies (WebGL2 cannot
//    map buffers; a null return from a stub would make the engine write to address 0,
//    which does not trap in wasm and silently corrupts static data)
//  - glGetTexLevelParameter from sizes and formats recorded at upload time
//  - desktop texture formats and parameters mapped to what WebGL2 accepts

#include "WebGLInternal.h"

#include <glad/glad.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "System/Log/ILog.h"

namespace {
	#define REAL(name) decltype(glad_##name) real_##name = nullptr
	#define HOOK(name) do { real_##name = glad_##name; glad_##name = compat_##name; } while (0)

	REAL(glGetIntegerv);
	REAL(glPixelStorei);
	REAL(glGetError);

	int numLoggedErrors = 0;
	// report the GL error a just-issued upload call raised (a few dozen at most)
	// WebGL errors stay set until read, so an error of an earlier unchecked call would be
	// reported by CheckError as this call's: read (and log) it first
	void ClearPendingError(const char* func) {
		const GLenum err = real_glGetError();
		if (err == GL_NO_ERROR || numLoggedErrors >= 60)
			return;
		numLoggedErrors++;
		LOG_L(L_WARNING, "[WebGLCompat] GL error 0x%x was pending before %s (raised by an earlier call)", err, func);
	}

	void CheckError(const char* func, GLenum target, GLint a, GLint b, GLint c, GLint w, GLint h) {
		const GLenum err = real_glGetError();
		if (err == GL_NO_ERROR || numLoggedErrors >= 60)
			return;
		numLoggedErrors++;
		LOG_L(L_WARNING, "[WebGLCompat] %s(target=0x%x, 0x%x, 0x%x, 0x%x, %dx%d) raised GL error 0x%x", func, target, a, b, c, w, h, err);
	}

	// ---- buffer mapping ---------------------------------------------------------------
	REAL(glGetBufferParameteriv);
	REAL(glBufferSubData);
	REAL(glGetBufferSubData);
	REAL(glDeleteBuffers);

	struct Mapping {
		uint8_t* ptr = nullptr;
		GLintptr offset = 0;
		GLsizeiptr length = 0;
		GLbitfield access = 0;
	};
	std::unordered_map<GLuint, Mapping> mappings; // by buffer name

	GLuint BoundBuffer(GLenum target) {
		GLenum binding = 0;
		switch (target) {
			case GL_ARRAY_BUFFER: binding = GL_ARRAY_BUFFER_BINDING; break;
			case GL_ELEMENT_ARRAY_BUFFER: binding = GL_ELEMENT_ARRAY_BUFFER_BINDING; break;
			case GL_PIXEL_PACK_BUFFER: binding = GL_PIXEL_PACK_BUFFER_BINDING; break;
			case GL_PIXEL_UNPACK_BUFFER: binding = GL_PIXEL_UNPACK_BUFFER_BINDING; break;
			case GL_UNIFORM_BUFFER: binding = GL_UNIFORM_BUFFER_BINDING; break;
			case GL_COPY_READ_BUFFER: binding = GL_COPY_READ_BUFFER_BINDING; break;
			case GL_COPY_WRITE_BUFFER: binding = GL_COPY_WRITE_BUFFER_BINDING; break;
			case GL_TRANSFORM_FEEDBACK_BUFFER: binding = GL_TRANSFORM_FEEDBACK_BUFFER_BINDING; break;
			default: return 0;
		}
		GLint id = 0;
		real_glGetIntegerv(binding, &id);
		return GLuint(id);
	}

	void* APIENTRY compat_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
		const GLuint buf = BoundBuffer(target);
		if (buf == 0 || length <= 0)
			return nullptr;

		Mapping& m = mappings[buf];
		std::free(m.ptr);
		m.ptr = static_cast<uint8_t*>(std::malloc(length));
		m.offset = offset;
		m.length = length;
		m.access = access;
		if (m.ptr == nullptr) {
			mappings.erase(buf);
			return nullptr;
		}

		// the whole range is uploaded at unmap, so its current contents are needed unless
		// the caller declared it will overwrite (or does not care about) them
		const GLbitfield overwrite = GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_FLUSH_EXPLICIT_BIT;
		if ((access & GL_MAP_READ_BIT) != 0 || (access & overwrite) == 0)
			real_glGetBufferSubData(target, offset, length, m.ptr);
		else
			std::memset(m.ptr, 0, length);
		return m.ptr;
	}

	void* APIENTRY compat_glMapBuffer(GLenum target, GLenum access) {
		GLint size = 0;
		real_glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
		switch (access) {
			case GL_READ_ONLY: return compat_glMapBufferRange(target, 0, size, GL_MAP_READ_BIT);
			case GL_READ_WRITE: return compat_glMapBufferRange(target, 0, size, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
			// write-only users fill the buffer; skip the readback
			default: return compat_glMapBufferRange(target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
		}
	}

	void APIENTRY compat_glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
		const auto it = mappings.find(BoundBuffer(target));
		if (it == mappings.end() || length <= 0 || offset < 0 || (offset + length) > it->second.length)
			return;
		real_glBufferSubData(target, it->second.offset + offset, length, it->second.ptr + offset);
	}

	GLboolean APIENTRY compat_glUnmapBuffer(GLenum target) {
		const auto it = mappings.find(BoundBuffer(target));
		if (it == mappings.end())
			return GL_FALSE;
		Mapping& m = it->second;
		if ((m.access & GL_MAP_WRITE_BIT) != 0 && (m.access & GL_MAP_FLUSH_EXPLICIT_BIT) == 0)
			real_glBufferSubData(target, m.offset, m.length, m.ptr);
		std::free(m.ptr);
		mappings.erase(it);
		return GL_TRUE;
	}

	void APIENTRY compat_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
		for (GLsizei i = 0; i < n; i++) {
			if (const auto it = mappings.find(buffers[i]); it != mappings.end()) {
				std::free(it->second.ptr);
				mappings.erase(it);
			}
		}
		real_glDeleteBuffers(n, buffers);
	}

	// ---- textures ------------------------------------------------------------------------
	REAL(glActiveTexture);
	REAL(glBindTexture);
	REAL(glDeleteTextures);
	REAL(glTexImage2D);
	REAL(glTexImage3D);
	REAL(glTexSubImage2D);
	REAL(glTexStorage2D);
	REAL(glTexStorage3D);
	REAL(glCompressedTexImage2D);
	REAL(glCopyTexImage2D);
	REAL(glTexParameteri);
	REAL(glTexParameterf);
	REAL(glTexParameteriv);
	REAL(glTexParameterfv);

	// alphaSplat: the engine set the (A, A, A, A) swizzle (font glyph atlas); WebGL2 has no
	// swizzles, so GL_ALPHA data for it is stored as LUMINANCE_ALPHA with L = A
	// rgbaStorage: an RGB float format the shim allocated as RGBA (see PromoteRGBFloat)
	struct TexInfo { GLint width = 0, height = 0, depth = 1; GLenum internalFormat = 0; bool compressed = false; bool alphaSplat = false; bool rgbaStorage = false; };
	std::unordered_map<GLuint, TexInfo> textures;

	constexpr int MAX_UNITS = 32;
	enum { TGT_2D, TGT_CUBE, TGT_3D, TGT_2D_ARRAY, TGT_COUNT };
	GLuint boundTex[MAX_UNITS][TGT_COUNT];
	int activeUnit = 0;

	int TargetIndex(GLenum target) {
		switch (target) {
			case GL_TEXTURE_2D: return TGT_2D;
			case GL_TEXTURE_3D: return TGT_3D;
			case GL_TEXTURE_2D_ARRAY: return TGT_2D_ARRAY;
			case GL_TEXTURE_CUBE_MAP: return TGT_CUBE;
			default: return (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)? TGT_CUBE: -1;
		}
	}
	TexInfo* BoundTexInfo(GLenum target) {
		const int idx = TargetIndex(target);
		if (idx < 0)
			return nullptr;
		const GLuint id = boundTex[activeUnit][idx];
		return (id != 0)? &textures[id]: nullptr;
	}
	void Record(GLenum target, GLint level, GLenum internalFormat, GLint w, GLint h, GLint d, bool compressed, bool rgbaStorage = false) {
		if (level != 0)
			return;
		if (TexInfo* t = BoundTexInfo(target); t != nullptr)
			*t = {w, h, d, internalFormat, compressed, t->alphaSplat, rgbaStorage};
	}

	void APIENTRY compat_glActiveTexture(GLenum texture) { activeUnit = std::clamp(int(texture - GL_TEXTURE0), 0, MAX_UNITS - 1); real_glActiveTexture(texture); }
	void APIENTRY compat_glBindTexture(GLenum target, GLuint texture) {
		if (WebGLCompat::listCompiling) {
			WebGLCompat::ListRecord([=] { compat_glBindTexture(target, texture); });
			return;
		}
		if (const int idx = TargetIndex(target); idx >= 0)
			boundTex[activeUnit][idx] = texture;
		real_glBindTexture(target, texture);
	}
	void APIENTRY compat_glDeleteTextures(GLsizei n, const GLuint* ids) {
		for (GLsizei i = 0; i < n; i++) {
			textures.erase(ids[i]);
			for (auto& unit: boundTex) {
				for (GLuint& b: unit) {
					if (b == ids[i])
						b = 0;
				}
			}
		}
		real_glDeleteTextures(n, ids);
	}

	// the (format, type) pair WebGL2 expects for a sized internal format; used both to
	// validate what the engine passed and to allocate storage when it passes no pixels
	struct FormatInfo { GLenum internalFormat, format, type; };
	const FormatInfo* SizedFormat(GLenum internalFormat) {
		static const FormatInfo table[] = {
			{GL_R8, GL_RED, GL_UNSIGNED_BYTE}, {GL_RG8, GL_RG, GL_UNSIGNED_BYTE},
			{GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE}, {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE},
			{GL_SRGB8, GL_RGB, GL_UNSIGNED_BYTE}, {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE},
			{GL_R16F, GL_RED, GL_HALF_FLOAT}, {GL_RG16F, GL_RG, GL_HALF_FLOAT},
			{GL_RGB16F, GL_RGB, GL_HALF_FLOAT}, {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT},
			{GL_R32F, GL_RED, GL_FLOAT}, {GL_RG32F, GL_RG, GL_FLOAT},
			{GL_RGB32F, GL_RGB, GL_FLOAT}, {GL_RGBA32F, GL_RGBA, GL_FLOAT},
			{GL_R11F_G11F_B10F, GL_RGB, GL_HALF_FLOAT}, {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV},
			{GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5}, {GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4}, {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1},
			{GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE}, {GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT}, {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT},
			{GL_R32I, GL_RED_INTEGER, GL_INT}, {GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE}, {GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT},
			{GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT},
			{GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT},
			{GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT},
			{GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8},
			{GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV},
		};
		for (const FormatInfo& f: table) {
			if (f.internalFormat == internalFormat)
				return &f;
		}
		return nullptr;
	}

	int NumComponents(GLenum format) {
		switch (format) {
			case GL_RED: case GL_RED_INTEGER: case GL_LUMINANCE: case GL_ALPHA: case GL_DEPTH_COMPONENT: return 1;
			case GL_RG: case GL_LUMINANCE_ALPHA: return 2;
			case GL_RGB: case GL_BGR: return 3;
			default: return 4;
		}
	}

	// desktop internal formats -> WebGL2 ones; may also rewrite format/type
	void FixFormats(GLint& internalFormat, GLenum& format, GLenum& type, bool havePixels) {
		// legacy component counts
		switch (internalFormat) {
			case 1: internalFormat = GL_LUMINANCE; break;
			case 2: internalFormat = GL_LUMINANCE_ALPHA; break;
			case 3: internalFormat = GL_RGB; break;
			case 4: internalFormat = GL_RGBA; break;
			default: break;
		}
		// sized/legacy formats that do not exist in GLES 3.0
		switch (internalFormat) {
			case GL_LUMINANCE8: case GL_INTENSITY: case GL_INTENSITY8: internalFormat = GL_LUMINANCE; break;
			case GL_ALPHA8: internalFormat = GL_ALPHA; break;
			case GL_LUMINANCE8_ALPHA8: internalFormat = GL_LUMINANCE_ALPHA; break;
			case GL_RGB5: internalFormat = GL_RGB8; break;
			case GL_RGB10: case GL_RGB12: case GL_RGB16: internalFormat = GL_RGB16F; break;
			case GL_RGBA12: case GL_RGBA16: internalFormat = GL_RGBA16F; break;
			// no EXT_texture_norm16 everywhere; R/RG hold heightmaps, so keep all 16 bits
			case GL_R16: internalFormat = GL_R32F; break;
			case GL_RG16: internalFormat = GL_RG32F; break;
			case GL_LUMINANCE16: case GL_LUMINANCE16F_ARB: internalFormat = GL_R16F; break;
			case GL_LUMINANCE32F_ARB: internalFormat = GL_R32F; break;
			case GL_DEPTH_COMPONENT32: internalFormat = GL_DEPTH_COMPONENT32F; break;
			case GL_DEPTH_COMPONENT: internalFormat = (type == GL_FLOAT)? GL_DEPTH_COMPONENT32F: ((type == GL_UNSIGNED_SHORT)? GL_DEPTH_COMPONENT16: GL_DEPTH_COMPONENT24); break;
			case GL_DEPTH_STENCIL: internalFormat = GL_DEPTH24_STENCIL8; break;
			default: break;
		}
		// unsized color formats with a float type need a sized float format
		if (type == GL_FLOAT || type == GL_HALF_FLOAT) {
			const bool half = (type == GL_HALF_FLOAT);
			switch (internalFormat) {
				case GL_RGBA: internalFormat = half? GL_RGBA16F: GL_RGBA32F; break;
				case GL_RGB: internalFormat = half? GL_RGB16F: GL_RGB32F; break;
				case GL_LUMINANCE: case GL_RED: internalFormat = half? GL_R16F: GL_R32F; break;
				default: break;
			}
		}

		const FormatInfo* sized = SizedFormat(internalFormat);
		if (sized != nullptr) {
			if (!havePixels) {
				// allocation only: any valid pair will do
				format = sized->format;
				type = sized->type;
			} else {
				// L/LA/A source data for an R/RG texture
				if (format == GL_LUMINANCE || format == GL_ALPHA) format = GL_RED;
				if (format == GL_LUMINANCE_ALPHA) format = GL_RG;
				// float data may go into half-float formats, but not the other way around
				if ((sized->type == GL_HALF_FLOAT || sized->type == GL_FLOAT) && type != GL_FLOAT && type != GL_HALF_FLOAT) {
					// normalized integer data for a format we mapped to float: the upload
					// converts it (NormalizedToFloat)
					format = sized->format;
				}
				if (internalFormat == GL_DEPTH_COMPONENT24 && type != GL_UNSIGNED_INT) type = GL_UNSIGNED_INT;
			}
		} else if (internalFormat == GL_RGB || internalFormat == GL_RGBA || internalFormat == GL_LUMINANCE || internalFormat == GL_ALPHA || internalFormat == GL_LUMINANCE_ALPHA) {
			// unsized: WebGL2 wants format == internalFormat
			if (!havePixels || NumComponents(format) == NumComponents(internalFormat))
				format = (format == GL_BGRA || format == GL_BGR)? format: GLenum(internalFormat);
			if (!havePixels)
				type = GL_UNSIGNED_BYTE;
		}
	}

	// RGB16F/RGB32F are not color-renderable in WebGL2 (EXT_color_buffer_float covers the
	// R, RG and RGBA float formats only), so a render target such as BAR's GenEnvLut LUT
	// (gl.CreateTexture{format = GL_RGB16F, fbo = true}) gets an incomplete framebuffer.
	// 2D storage allocated without pixels is made RGBA instead. Sampling it gives the same
	// RGB; alpha holds whatever was rendered rather than 1. Later RGB uploads are widened
	// (WidenRGBFloat). Returns true if the format was changed.
	bool PromoteRGBFloat(GLint& internalFormat, GLenum& format) {
		if (internalFormat != GL_RGB16F && internalFormat != GL_RGB32F)
			return false;
		internalFormat = (internalFormat == GL_RGB16F)? GL_RGBA16F: GL_RGBA32F;
		format = GL_RGBA;
		return true;
	}

	// RGB float/half-float rows -> RGBA with alpha 1, for an rgbaStorage texture
	std::vector<uint8_t> WidenRGBFloat(GLenum& format, GLenum type, GLsizei w, GLsizei h, const void* pixels) {
		std::vector<uint8_t> out;
		if (format != GL_RGB || pixels == nullptr || (type != GL_FLOAT && type != GL_HALF_FLOAT))
			return out;
		const size_t bpc = (type == GL_FLOAT)? 4: 2;
		GLint align = 4;
		real_glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
		GLint rowLength = 0;
		real_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
		const size_t inRow = (size_t((rowLength > 0)? rowLength: w) * 3 * bpc + align - 1) / align * align;
		const size_t outRow = (size_t(w) * 4 * bpc + align - 1) / align * align;
		out.assign(outRow * size_t(h), 0);
		const float one = 1.0f;
		const uint16_t halfOne = 0x3c00;
		const uint8_t* src = static_cast<const uint8_t*>(pixels);
		for (GLsizei y = 0; y < h; y++) {
			for (GLsizei x = 0; x < w; x++) {
				uint8_t* o = out.data() + size_t(y) * outRow + size_t(x) * 4 * bpc;
				std::memcpy(o, src + size_t(y) * inRow + size_t(x) * 3 * bpc, 3 * bpc);
				std::memcpy(o + 3 * bpc, (bpc == 4)? static_cast<const void*>(&one): static_cast<const void*>(&halfOne), bpc);
			}
		}
		format = GL_RGBA;
		if (rowLength > 0)
			real_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		return out;
	}

	// BGR(A) byte data -> RGB(A); returns the converted copy (empty if not needed)
	std::vector<uint8_t> SwapBGR(GLenum& format, GLenum type, GLsizei w, GLsizei h, GLsizei d, const void* pixels) {
		std::vector<uint8_t> out;
		if ((format != GL_BGRA && format != GL_BGR) || type != GL_UNSIGNED_BYTE)
			return out;
		const int n = (format == GL_BGRA)? 4: 3;
		format = (n == 4)? GL_RGBA: GL_RGB;
		if (pixels == nullptr)
			return out;
		GLint align = 4;
		real_glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
		const size_t rowBytes = (size_t(w) * n + align - 1) / align * align;
		out.resize(rowBytes * h * d);
		std::memcpy(out.data(), pixels, out.size());
		for (size_t row = 0; row < size_t(h) * d; row++) {
			uint8_t* p = out.data() + row * rowBytes;
			for (GLsizei x = 0; x < w; x++, p += n)
				std::swap(p[0], p[2]);
		}
		return out;
	}

	// normalized unsigned byte/short data for a texture the shim gave a float format (GL_R16
	// -> GL_R32F, ...): converted to GL_FLOAT; returns the copy (empty if not needed)
	std::vector<float> NormalizedToFloat(GLint internalFormat, GLenum format, GLenum& type, GLsizei w, GLsizei h, GLsizei d, const void* pixels) {
		std::vector<float> out;
		const FormatInfo* sized = SizedFormat(internalFormat);
		if (pixels == nullptr || sized == nullptr || (sized->type != GL_FLOAT && sized->type != GL_HALF_FLOAT))
			return out;
		if (type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_BYTE)
			return out;
		const size_t n = size_t(NumComponents(format)) * size_t(w);
		const size_t bpc = (type == GL_UNSIGNED_SHORT)? 2: 1;
		GLint align = 4;
		real_glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
		GLint rowLength = 0;
		real_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
		const size_t rowElems = (rowLength > 0)? size_t(NumComponents(format)) * size_t(rowLength): n;
		const size_t rowBytes = (rowElems * bpc + align - 1) / align * align;
		out.resize(n * size_t(h) * size_t(d));
		const uint8_t* src = static_cast<const uint8_t*>(pixels);
		for (size_t row = 0; row < size_t(h) * size_t(d); row++) {
			const uint8_t* r = src + row * rowBytes;
			float* o = out.data() + row * n;
			if (bpc == 2) {
				for (size_t i = 0; i < n; i++) { uint16_t v; std::memcpy(&v, r + i * 2, 2); o[i] = v / 65535.0f; }
			} else {
				for (size_t i = 0; i < n; i++) o[i] = r[i] / 255.0f;
			}
		}
		type = GL_FLOAT;
		// the converted rows are tightly packed
		if (rowLength > 0)
			real_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		return out;
	}
	struct RowLengthRestore {
		GLint v = 0;
		RowLengthRestore() { real_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &v); }
		~RowLengthRestore() { if (v > 0) real_glPixelStorei(GL_UNPACK_ROW_LENGTH, v); }
	};

	// GL_ALPHA bytes -> LUMINANCE_ALPHA (L = A) for an alphaSplat texture; returns the
	// converted copy (empty if not needed; also when there are no pixels, only the format changes)
	std::vector<uint8_t> AlphaSplat(GLenum target, GLint& internalFormat, GLenum& format, GLenum type, GLsizei w, GLsizei h, const void* pixels, bool pbo) {
		std::vector<uint8_t> out;
		const TexInfo* t = BoundTexInfo(target);
		if (t == nullptr || !t->alphaSplat || format != GL_ALPHA || type != GL_UNSIGNED_BYTE || pbo)
			return out;
		if (internalFormat == GL_ALPHA || internalFormat == GL_ALPHA8)
			internalFormat = GL_LUMINANCE_ALPHA;
		format = GL_LUMINANCE_ALPHA;
		if (pixels == nullptr)
			return out;
		GLint align = 4;
		real_glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
		const size_t inRow = (size_t(w) + align - 1) / align * align;
		const size_t outRow = (size_t(w) * 2 + align - 1) / align * align;
		out.resize(outRow * size_t(h));
		const uint8_t* src = static_cast<const uint8_t*>(pixels);
		for (GLsizei y = 0; y < h; y++) {
			const uint8_t* r = src + size_t(y) * inRow;
			uint8_t* o = out.data() + size_t(y) * outRow;
			for (GLsizei x = 0; x < w; x++)
				o[x * 2] = o[x * 2 + 1] = r[x];
		}
		return out;
	}

	// a pending GL error from some earlier call would otherwise be blamed on the upload
	void DrainErrors() { for (int i = 0; i < 8 && real_glGetError() != GL_NO_ERROR; i++) {} }

	bool UnpackBufferBound() {
		DrainErrors();
		GLint id = 0;
		real_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &id);
		return (id != 0);
	}

	void APIENTRY compat_glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void* pixels) {
		ClearPendingError("glTexImage2D");
		const bool pbo = UnpackBufferBound();
		const GLint origInternal = internalFormat;
		const std::vector<uint8_t> splat = AlphaSplat(target, internalFormat, format, type, w, h, pixels, pbo);
		FixFormats(internalFormat, format, type, pixels != nullptr || pbo);
		const bool rgbaStorage = (pixels == nullptr && !pbo && PromoteRGBFloat(internalFormat, format));
		const RowLengthRestore rowLength;
		const std::vector<float> floats = pbo? std::vector<float>(): NormalizedToFloat(internalFormat, format, type, w, h, 1, pixels);
		const std::vector<uint8_t> swapped = pbo? std::vector<uint8_t>(): SwapBGR(format, type, w, h, 1, pixels);
		const void* data = !floats.empty()? static_cast<const void*>(floats.data()): (!splat.empty()? splat.data(): (swapped.empty()? pixels: swapped.data()));
		real_glTexImage2D(target, level, internalFormat, w, h, 0, format, type, data);
		CheckError("glTexImage2D", target, origInternal, format, type, w, h);
		Record(target, level, origInternal, w, h, 1, false, rgbaStorage);
	}

	void APIENTRY compat_glTexImage3D(GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum format, GLenum type, const void* pixels) {
		ClearPendingError("glTexImage3D");
		const bool pbo = UnpackBufferBound();
		const GLint origInternal = internalFormat;
		FixFormats(internalFormat, format, type, pixels != nullptr || pbo);
		const RowLengthRestore rowLength;
		const std::vector<float> floats = pbo? std::vector<float>(): NormalizedToFloat(internalFormat, format, type, w, h, d, pixels);
		const std::vector<uint8_t> swapped = pbo? std::vector<uint8_t>(): SwapBGR(format, type, w, h, d, pixels);
		const void* data = !floats.empty()? static_cast<const void*>(floats.data()): (swapped.empty()? pixels: swapped.data());
		real_glTexImage3D(target, level, internalFormat, w, h, d, 0, format, type, data);
		CheckError("glTexImage3D", target, origInternal, format, type, w, h);
		Record(target, level, origInternal, w, h, d, false);
	}

	void APIENTRY compat_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels) {
		const bool pbo = UnpackBufferBound();
		const RowLengthRestore rowLength;
		std::vector<float> floats;
		GLint splatInternal = GL_ALPHA;
		const std::vector<uint8_t> splat = AlphaSplat(target, splatInternal, format, type, w, h, pixels, pbo);
		if (const TexInfo* t = BoundTexInfo(target); t != nullptr && splat.empty() && format != GL_LUMINANCE_ALPHA) {
			// follow the mapping the texture's storage got
			GLint internalFormat = GLint(t->internalFormat);
			GLenum f = format, ty = type;
			FixFormats(internalFormat, f, ty, true);
			if (SizedFormat(internalFormat) != nullptr || f == GLenum(internalFormat))
				format = (format == GL_BGRA || format == GL_BGR)? format: f;
			type = ty;
			if (!pbo)
				floats = NormalizedToFloat(internalFormat, format, type, w, h, 1, pixels);
		}
		const std::vector<uint8_t> swapped = pbo? std::vector<uint8_t>(): SwapBGR(format, type, w, h, 1, pixels);
		std::vector<uint8_t> widened;
		if (const TexInfo* t = BoundTexInfo(target); t != nullptr && t->rgbaStorage && !pbo && floats.empty())
			widened = WidenRGBFloat(format, type, w, h, swapped.empty()? pixels: swapped.data());
		const void* data = !floats.empty()? static_cast<const void*>(floats.data()): (!splat.empty()? splat.data(): (!widened.empty()? widened.data(): (swapped.empty()? pixels: swapped.data())));
		real_glTexSubImage2D(target, level, x, y, w, h, format, type, data);
		CheckError("glTexSubImage2D", target, level, format, type, w, h);
	}

	void APIENTRY compat_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei w, GLsizei h) {
		ClearPendingError("glTexStorage2D");
		GLint fixedFormat = GLint(internalFormat);
		GLenum f = GL_RGBA, t = GL_UNSIGNED_BYTE;
		FixFormats(fixedFormat, f, t, false);
		const bool rgbaStorage = PromoteRGBFloat(fixedFormat, f);
		DrainErrors();
		real_glTexStorage2D(target, levels, fixedFormat, w, h);
		CheckError("glTexStorage2D", target, internalFormat, levels, 0, w, h);
		Record(target, 0, internalFormat, w, h, 1, false, rgbaStorage);
	}

	void APIENTRY compat_glTexStorage3D(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei w, GLsizei h, GLsizei d) {
		ClearPendingError("glTexStorage3D");
		GLint fixedFormat = GLint(internalFormat);
		GLenum f = GL_RGBA, t = GL_UNSIGNED_BYTE;
		FixFormats(fixedFormat, f, t, false);
		real_glTexStorage3D(target, levels, fixedFormat, w, h, d);
		CheckError("glTexStorage3D", target, internalFormat, levels, 0, w, h);
		Record(target, 0, internalFormat, w, h, d, false);
	}

	void APIENTRY compat_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLsizei w, GLsizei h, GLint border, GLsizei imageSize, const void* data) {
		ClearPendingError("glCompressedTexImage2D");
		real_glCompressedTexImage2D(target, level, internalFormat, w, h, 0, imageSize, data);
		CheckError("glCompressedTexImage2D", target, internalFormat, level, imageSize, w, h);
		Record(target, level, internalFormat, w, h, 1, true);
	}

	void APIENTRY compat_glCopyTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei w, GLsizei h, GLint border) {
		ClearPendingError("glCopyTexImage2D");
		real_glCopyTexImage2D(target, level, internalFormat, x, y, w, h, 0);
		CheckError("glCopyTexImage2D", target, internalFormat, level, 0, w, h);
		Record(target, level, internalFormat, w, h, 1, false);
	}

	// ---- renderbuffers ----------------------------------------------------------------
	// WebGL2 renderbuffers need sized formats. Desktop GL accepts unsized ones: the engine's
	// reflection cubemap (CubeMapHandler) asks for GL_DEPTH_COMPONENT, which WebGL rejects,
	// leaving the depth attachment without storage and the FBO incomplete, so every face
	// render failed. RGB float formats are not renderable in WebGL2 (EXT_color_buffer_float
	// covers R/RG/RGBA only), as for textures. GL_DEPTH_COMPONENT32 stays unmapped: the
	// engine probes it to choose its depth precision, and 24 bits is what it then uses.
	REAL(glRenderbufferStorage);
	REAL(glRenderbufferStorageEXT);
	REAL(glRenderbufferStorageMultisample);
	REAL(glRenderbufferStorageMultisampleEXT);

	GLenum RenderbufferFormat(GLenum internalFormat) {
		switch (internalFormat) {
			case GL_DEPTH_COMPONENT: return GL_DEPTH_COMPONENT24;
			case GL_DEPTH_STENCIL: return GL_DEPTH24_STENCIL8;
			case GL_RGBA: return GL_RGBA8;
			case GL_RGB: return GL_RGB8;
			case GL_RGB16F: return GL_RGBA16F;
			case GL_RGB32F: return GL_RGBA32F;
			default: return internalFormat;
		}
	}

	void APIENTRY compat_glRenderbufferStorage(GLenum target, GLenum internalFormat, GLsizei w, GLsizei h) {
		real_glRenderbufferStorage(target, RenderbufferFormat(internalFormat), w, h);
	}
	void APIENTRY compat_glRenderbufferStorageEXT(GLenum target, GLenum internalFormat, GLsizei w, GLsizei h) {
		real_glRenderbufferStorageEXT(target, RenderbufferFormat(internalFormat), w, h);
	}
	void APIENTRY compat_glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalFormat, GLsizei w, GLsizei h) {
		real_glRenderbufferStorageMultisample(target, samples, RenderbufferFormat(internalFormat), w, h);
	}
	void APIENTRY compat_glRenderbufferStorageMultisampleEXT(GLenum target, GLsizei samples, GLenum internalFormat, GLsizei w, GLsizei h) {
		real_glRenderbufferStorageMultisampleEXT(target, samples, RenderbufferFormat(internalFormat), w, h);
	}

	// ---- depth blits ---------------------------------------------------------------------
	// WebGL2 blits depth only between identical depth/stencil formats. The default framebuffer
	// has a 24-bit depth and an 8-bit stencil buffer (the context is created with both), while
	// the engine's depth copies (DepthBufferCopy, read by ground decals and soft particles)
	// are DEPTH_COMPONENT24 textures: every copy failed with INVALID_OPERATION and the copy
	// stayed empty. Before a depth blit between the default framebuffer and an FBO whose depth
	// attachment is such a texture, that texture's storage is re-specified as DEPTH24_STENCIL8
	// (same name, size and level; it stays attached and samples the same depth values). This
	// happens once per texture: afterwards the formats match.
	REAL(glBlitFramebuffer);
	REAL(glBlitFramebufferEXT);

	GLint AttachmentParam(GLenum target, GLenum attachment, GLenum pname) {
		GLint v = 0;
		glGetFramebufferAttachmentParameteriv(target, attachment, pname, &v);
		return v;
	}

	void MatchDefaultDepthFormat(GLbitfield mask) {
		if ((mask & GL_DEPTH_BUFFER_BIT) == 0)
			return;
		GLint readFbo = 0, drawFbo = 0;
		real_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
		real_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
		if ((readFbo == 0) == (drawFbo == 0))
			return;
		const GLenum defaultTarget = (readFbo == 0)? GL_READ_FRAMEBUFFER: GL_DRAW_FRAMEBUFFER;
		const GLenum fboTarget = (readFbo == 0)? GL_DRAW_FRAMEBUFFER: GL_READ_FRAMEBUFFER;

		if (AttachmentParam(defaultTarget, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE) != 24)
			return;
		if (AttachmentParam(defaultTarget, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE) != 8)
			return;
		if (AttachmentParam(fboTarget, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) != GL_TEXTURE)
			return;
		if (AttachmentParam(fboTarget, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE) != 24)
			return;
		if (AttachmentParam(fboTarget, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE) != 0)
			return;
		if (AttachmentParam(fboTarget, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE) != 0)
			return;

		const GLuint tex = GLuint(AttachmentParam(fboTarget, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME));
		const GLint level = AttachmentParam(fboTarget, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL);
		const auto it = textures.find(tex);
		if (it == textures.end() || it->second.depth != 1)
			return;
		const GLint w = std::max(1, it->second.width >> level);
		const GLint h = std::max(1, it->second.height >> level);

		GLint unpackBuf = 0;
		real_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuf);
		if (unpackBuf != 0)
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		real_glBindTexture(GL_TEXTURE_2D, tex);
		GLint immutable = 0;
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
		if (immutable == 0)
			real_glTexImage2D(GL_TEXTURE_2D, level, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
		real_glBindTexture(GL_TEXTURE_2D, boundTex[activeUnit][TGT_2D]);
		if (unpackBuf != 0)
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, GLuint(unpackBuf));

		static int numLogged = 0;
		if (numLogged++ < 8) {
			LOG("[WebGLCompat] depth texture %u (%dx%d, level %d) %s to match the default framebuffer for depth blits",
				tex, w, h, level, (immutable == 0)? "re-specified as DEPTH24_STENCIL8": "is immutable and was left as DEPTH_COMPONENT24");
		}
	}

	void APIENTRY compat_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
		MatchDefaultDepthFormat(mask);
		real_glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
	}
	void APIENTRY compat_glBlitFramebufferEXT(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
		MatchDefaultDepthFormat(mask);
		real_glBlitFramebufferEXT(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
	}

	// ---- depth copies (glCopyTexSubImage2D into a depth texture) -------------------------
	// Desktop GL copies the read framebuffer's depth into a depth texture this way (Lua
	// gl.CopyToTexture: BAR's depth of field and volumetric clouds). WebGL2 only copies
	// colour. The copy becomes a depth blit into a scratch FBO with the texture attached;
	// as for engine blits, a texture whose depth/stencil layout doesn't match the default
	// framebuffer's 24/8 is re-specified as DEPTH24_STENCIL8 first (once per texture).
	REAL(glCopyTexSubImage2D);
	GLuint depthCopyFbo = 0;
	uint32_t depthCopies = 0;

	bool IsDepthFormat(GLenum f) {
		switch (f) {
			case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32:
			case GL_DEPTH_COMPONENT32F: case GL_DEPTH_STENCIL: case GL_DEPTH24_STENCIL8: case GL_DEPTH32F_STENCIL8:
				return true;
			default:
				return false;
		}
	}

	bool CopyDepthToTexture(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h) {
		if (target != GL_TEXTURE_2D)
			return false;
		const GLuint tex = boundTex[activeUnit][TGT_2D];
		const auto it = textures.find(tex);
		if (tex == 0 || it == textures.end() || !IsDepthFormat(it->second.internalFormat))
			return false;

		GLint readFbo = 0, drawFbo = 0;
		real_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
		real_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
		if (depthCopyFbo == 0)
			glGenFramebuffers(1, &depthCopyFbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, depthCopyFbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, level);

		// the read side's layout; for the default framebuffer that's 24/8
		const GLenum readAtt = (readFbo == 0)? GL_DEPTH: GL_DEPTH_ATTACHMENT;
		const GLenum readStencilAtt = (readFbo == 0)? GL_STENCIL: GL_DEPTH_ATTACHMENT;
		const GLint srcDepth = AttachmentParam(GL_READ_FRAMEBUFFER, readAtt, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE);
		const GLint srcStencil = AttachmentParam(GL_READ_FRAMEBUFFER, readStencilAtt, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE);
		const GLint dstDepth = AttachmentParam(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE);
		const GLint dstStencil = AttachmentParam(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE);

		if (srcDepth == 24 && srcStencil == 8 && (dstDepth != 24 || dstStencil != 8)) {
			GLint immutable = 0;
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
			if (immutable == 0) {
				GLint unpackBuf = 0;
				real_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuf);
				if (unpackBuf != 0)
					glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
				const GLint lw = std::max(1, it->second.width >> level);
				const GLint lh = std::max(1, it->second.height >> level);
				real_glTexImage2D(GL_TEXTURE_2D, level, GL_DEPTH24_STENCIL8, lw, lh, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
				if (unpackBuf != 0)
					glBindBuffer(GL_PIXEL_UNPACK_BUFFER, GLuint(unpackBuf));
				glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, level);
				static int numLogged = 0;
				if (numLogged++ < 8)
					LOG("[WebGLCompat] depth copy: texture %u (%dx%d, 0x%x) re-specified as DEPTH24_STENCIL8 to match the read framebuffer", tex, lw, lh, it->second.internalFormat);
			}
		}

		// glBlitFramebuffer honours the scissor test; glCopyTexSubImage2D's source rect doesn't
		const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
		if (scissor)
			glDisable(GL_SCISSOR_TEST);
		real_glBlitFramebuffer(x, y, x + w, y + h, xoff, yoff, xoff + w, yoff + h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
		if (scissor)
			glEnable(GL_SCISSOR_TEST);

		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFbo));
		depthCopies++;
		return true;
	}

	void APIENTRY compat_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h) {
		if (CopyDepthToTexture(target, level, xoff, yoff, x, y, w, h))
			return;
		real_glCopyTexSubImage2D(target, level, xoff, yoff, x, y, w, h);
	}

	void APIENTRY compat_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params) {
		*params = 0;
		const TexInfo* t = BoundTexInfo(target);
		if (t == nullptr)
			return;
		switch (pname) {
			case GL_TEXTURE_WIDTH: *params = (t->width > 0)? std::max(1, t->width >> level): 0; break;
			case GL_TEXTURE_HEIGHT: *params = (t->height > 0)? std::max(1, t->height >> level): 0; break;
			case GL_TEXTURE_DEPTH: *params = (target == GL_TEXTURE_3D)? std::max(1, t->depth >> level): t->depth; break;
			case GL_TEXTURE_INTERNAL_FORMAT: *params = GLint(t->internalFormat); break;
			case GL_TEXTURE_COMPRESSED: *params = t->compressed; break;
			default: break;
		}
	}
	void APIENTRY compat_glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat* params) {
		GLint v = 0;
		compat_glGetTexLevelParameteriv(target, level, pname, &v);
		*params = float(v);
	}

	// texture parameters: wrap modes and parameters GLES 3.0 lacks
	bool FixTexParam(GLenum pname, GLint& param) {
		switch (pname) {
			case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T: case GL_TEXTURE_WRAP_R:
				// no border color in WebGL2: clamp to the edge texel instead
				if (param == GL_CLAMP || param == GL_CLAMP_TO_BORDER)
					param = GL_CLAMP_TO_EDGE;
				return true;
			case GL_TEXTURE_MIN_FILTER: case GL_TEXTURE_MAG_FILTER:
			case GL_TEXTURE_BASE_LEVEL: case GL_TEXTURE_MAX_LEVEL:
			case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_MAX_LOD:
			case GL_TEXTURE_COMPARE_MODE: case GL_TEXTURE_COMPARE_FUNC:
			case GL_TEXTURE_MAX_ANISOTROPY_EXT:
				return true;
			default:
				// GL_GENERATE_MIPMAP, GL_TEXTURE_LOD_BIAS, GL_DEPTH_TEXTURE_MODE, border color, swizzles, ...
				return false;
		}
	}
	// desktop GL has a default texture object (name 0) whose parameters can be set;
	// WebGL2 has none and raises INVALID_OPERATION. The engine resets sampler state
	// after binding 0 (CShadowHandler::ResetShadowTexSampler), which only touches that
	// default object, so dropping the call there changes nothing that is ever sampled.
	bool NoTextureBound(GLenum target) {
		const int idx = TargetIndex(target);
		return (idx >= 0 && !WebGLCompat::listCompiling && boundTex[activeUnit][idx] == 0);
	}
	void APIENTRY compat_glTexParameteri(GLenum target, GLenum pname, GLint param) {
		if (NoTextureBound(target))
			return;
		if (FixTexParam(pname, param))
			real_glTexParameteri(target, pname, param);
	}
	void APIENTRY compat_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
		if (NoTextureBound(target))
			return;
		GLint i = GLint(param);
		if (!FixTexParam(pname, i))
			return;
		if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R)
			real_glTexParameteri(target, pname, i);
		else
			real_glTexParameterf(target, pname, param);
	}
	void APIENTRY compat_glTexParameteriv(GLenum target, GLenum pname, const GLint* params) {
		if (pname == GL_TEXTURE_SWIZZLE_RGBA) {
			if (TexInfo* t = BoundTexInfo(target); t != nullptr)
				t->alphaSplat = (params[0] == GL_ALPHA && params[1] == GL_ALPHA && params[2] == GL_ALPHA && params[3] == GL_ALPHA);
			return;
		}
		if (NoTextureBound(target))
			return;
		GLint p = params[0];
		if (FixTexParam(pname, p))
			real_glTexParameteri(target, pname, p);
	}
	void APIENTRY compat_glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params) { compat_glTexParameterf(target, pname, params[0]); }

	// WebGL2 can't generate mipmaps for compressed textures (INVALID_OPERATION). The
	// engine asks for them on DDS files without embedded levels (CBitmap::CreateDDSTexture,
	// S3O model textures) after setting a mipmapped min filter, which leaves a single-level
	// texture incomplete: it samples as black. Limit such textures to level 0 instead, so
	// they stay complete under any min filter (no minification smoothing, but correct texels).
	REAL(glGenerateMipmap);
	REAL(glGenerateMipmapEXT);
	bool CompressedMipmapFallback(GLenum target) {
		const TexInfo* t = BoundTexInfo(target);
		if (t == nullptr || !t->compressed)
			return false;
		real_glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
		static int logged = 0;
		if (logged++ < 4)
			LOG_L(L_WARNING, "[WebGL] glGenerateMipmap on a compressed texture (%dx%d, 0x%x): limited to level 0", t->width, t->height, t->internalFormat);
		return true;
	}
	void APIENTRY compat_glGenerateMipmap(GLenum target) { if (!CompressedMipmapFallback(target)) real_glGenerateMipmap(target); }
	void APIENTRY compat_glGenerateMipmapEXT(GLenum target) { if (!CompressedMipmapFallback(target)) real_glGenerateMipmapEXT(target); }

	// GL_FLOAT readback (Lua's gl.ReadPixels) of a framebuffer WebGL2 only reads as
	// RGBA/UNSIGNED_BYTE: read bytes and convert
	REAL(glReadPixels);
	void APIENTRY compat_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
		if (type != GL_FLOAT || pixels == nullptr || w <= 0 || h <= 0 || format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL) {
			real_glReadPixels(x, y, w, h, format, type, pixels);
			return;
		}
		GLint readType = 0;
		real_glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &readType);
		GLint packBuf = 0;
		real_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuf);
		if (readType == GL_FLOAT || packBuf != 0) {
			real_glReadPixels(x, y, w, h, format, type, pixels);
			return;
		}
		int comps[4] = {0, 1, 2, 3};
		int n = 4;
		switch (format) {
			case GL_RGB: n = 3; break;
			case GL_RED: case GL_LUMINANCE: n = 1; break;
			case GL_GREEN: n = 1; comps[0] = 1; break;
			case GL_BLUE: n = 1; comps[0] = 2; break;
			case GL_ALPHA: n = 1; comps[0] = 3; break;
			case GL_RG: n = 2; break;
			case GL_BGRA: comps[0] = 2; comps[2] = 0; break;
			case GL_BGR: n = 3; comps[0] = 2; comps[2] = 0; break;
			default: break;
		}
		GLint align = 4;
		real_glGetIntegerv(GL_PACK_ALIGNMENT, &align);
		real_glPixelStorei(GL_PACK_ALIGNMENT, 4);
		std::vector<uint8_t> bytes(size_t(w) * h * 4);
		real_glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
		real_glPixelStorei(GL_PACK_ALIGNMENT, align);
		float* out = static_cast<float*>(pixels);
		for (size_t i = 0; i < size_t(w) * h; i++) {
			for (int c = 0; c < n; c++)
				out[i * n + c] = bytes[i * 4 + comps[c]] / 255.0f;
		}
	}
}


void WebGLCompat::InstallObjectEmulation()
{
	real_glGetIntegerv = glad_glGetIntegerv;
	real_glPixelStorei = glad_glPixelStorei;
	real_glGetError = glad_glGetError;
	real_glGetBufferParameteriv = glad_glGetBufferParameteriv;
	real_glBufferSubData = glad_glBufferSubData;
	real_glGetBufferSubData = glad_glGetBufferSubData;
	real_glTexStorage2D = glad_glTexStorage2D;

	glad_glMapBuffer = compat_glMapBuffer;
	glad_glMapBufferRange = compat_glMapBufferRange;
	glad_glFlushMappedBufferRange = compat_glFlushMappedBufferRange;
	glad_glUnmapBuffer = compat_glUnmapBuffer;
	HOOK(glDeleteBuffers);

	HOOK(glActiveTexture); HOOK(glBindTexture); HOOK(glDeleteTextures);
	HOOK(glTexImage2D); HOOK(glTexImage3D); HOOK(glTexSubImage2D);
	HOOK(glTexStorage2D); HOOK(glTexStorage3D);
	HOOK(glCompressedTexImage2D); HOOK(glCopyTexImage2D);
	// nv_dds (DDS model/feature textures) uploads through the ARB alias, a separate glad
	// pointer: route it through the same bookkeeping (compressed flag for glGenerateMipmap)
	glad_glCompressedTexImage2DARB = compat_glCompressedTexImage2D;
	HOOK(glTexParameteri); HOOK(glTexParameterf); HOOK(glTexParameteriv); HOOK(glTexParameterfv);
	HOOK(glGenerateMipmap); HOOK(glGenerateMipmapEXT);
	glad_glGetTexLevelParameteriv = compat_glGetTexLevelParameteriv;
	glad_glGetTexLevelParameterfv = compat_glGetTexLevelParameterfv;
	HOOK(glReadPixels);
	HOOK(glRenderbufferStorage); HOOK(glRenderbufferStorageEXT);
	HOOK(glRenderbufferStorageMultisample); HOOK(glRenderbufferStorageMultisampleEXT);
	HOOK(glBlitFramebuffer); HOOK(glBlitFramebufferEXT);
	HOOK(glCopyTexSubImage2D);
}
