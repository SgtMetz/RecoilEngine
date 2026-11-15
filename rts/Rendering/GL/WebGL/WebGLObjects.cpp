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
	struct TexInfo { GLint width = 0, height = 0, depth = 1; GLenum internalFormat = 0; bool compressed = false; bool alphaSplat = false; };
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
	void Record(GLenum target, GLint level, GLenum internalFormat, GLint w, GLint h, GLint d, bool compressed) {
		if (level != 0)
			return;
		if (TexInfo* t = BoundTexInfo(target); t != nullptr)
			*t = {w, h, d, internalFormat, compressed, t->alphaSplat};
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
		const bool pbo = UnpackBufferBound();
		const GLint origInternal = internalFormat;
		const std::vector<uint8_t> splat = AlphaSplat(target, internalFormat, format, type, w, h, pixels, pbo);
		FixFormats(internalFormat, format, type, pixels != nullptr || pbo);
		const RowLengthRestore rowLength;
		const std::vector<float> floats = pbo? std::vector<float>(): NormalizedToFloat(internalFormat, format, type, w, h, 1, pixels);
		const std::vector<uint8_t> swapped = pbo? std::vector<uint8_t>(): SwapBGR(format, type, w, h, 1, pixels);
		const void* data = !floats.empty()? static_cast<const void*>(floats.data()): (!splat.empty()? splat.data(): (swapped.empty()? pixels: swapped.data()));
		real_glTexImage2D(target, level, internalFormat, w, h, 0, format, type, data);
		CheckError("glTexImage2D", target, origInternal, format, type, w, h);
		Record(target, level, origInternal, w, h, 1, false);
	}

	void APIENTRY compat_glTexImage3D(GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum format, GLenum type, const void* pixels) {
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
		const void* data = !floats.empty()? static_cast<const void*>(floats.data()): (!splat.empty()? splat.data(): (swapped.empty()? pixels: swapped.data()));
		real_glTexSubImage2D(target, level, x, y, w, h, format, type, data);
		CheckError("glTexSubImage2D", target, level, format, type, w, h);
	}

	void APIENTRY compat_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei w, GLsizei h) {
		GLint fixedFormat = GLint(internalFormat);
		GLenum f = GL_RGBA, t = GL_UNSIGNED_BYTE;
		FixFormats(fixedFormat, f, t, false);
		DrainErrors();
		real_glTexStorage2D(target, levels, fixedFormat, w, h);
		CheckError("glTexStorage2D", target, internalFormat, levels, 0, w, h);
		Record(target, 0, internalFormat, w, h, 1, false);
	}

	void APIENTRY compat_glTexStorage3D(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei w, GLsizei h, GLsizei d) {
		GLint fixedFormat = GLint(internalFormat);
		GLenum f = GL_RGBA, t = GL_UNSIGNED_BYTE;
		FixFormats(fixedFormat, f, t, false);
		real_glTexStorage3D(target, levels, fixedFormat, w, h, d);
		CheckError("glTexStorage3D", target, internalFormat, levels, 0, w, h);
		Record(target, 0, internalFormat, w, h, d, false);
	}

	void APIENTRY compat_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLsizei w, GLsizei h, GLint border, GLsizei imageSize, const void* data) {
		real_glCompressedTexImage2D(target, level, internalFormat, w, h, 0, imageSize, data);
		CheckError("glCompressedTexImage2D", target, internalFormat, level, imageSize, w, h);
		Record(target, level, internalFormat, w, h, 1, true);
	}

	void APIENTRY compat_glCopyTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei w, GLsizei h, GLint border) {
		real_glCopyTexImage2D(target, level, internalFormat, x, y, w, h, 0);
		CheckError("glCopyTexImage2D", target, internalFormat, level, 0, w, h);
		Record(target, level, internalFormat, w, h, 1, false);
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
	void APIENTRY compat_glTexParameteri(GLenum target, GLenum pname, GLint param) { if (FixTexParam(pname, param)) real_glTexParameteri(target, pname, param); }
	void APIENTRY compat_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
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
		GLint p = params[0];
		if (FixTexParam(pname, p))
			real_glTexParameteri(target, pname, p);
	}
	void APIENTRY compat_glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params) { compat_glTexParameterf(target, pname, params[0]); }

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
	HOOK(glTexParameteri); HOOK(glTexParameterf); HOOK(glTexParameteriv); HOOK(glTexParameterfv);
	glad_glGetTexLevelParameteriv = compat_glGetTexLevelParameteriv;
	glad_glGetTexLevelParameterfv = compat_glGetTexLevelParameterfv;
	HOOK(glReadPixels);
}
