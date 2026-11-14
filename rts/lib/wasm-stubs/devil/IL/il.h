/* WASM port stand-in for DevIL's <IL/il.h>.
 *
 * Used only when building for Emscripten without a real DevIL (WASM_NO_DEVIL).
 * Implements just the subset of DevIL that rts/Rendering/Textures/Bitmap.cpp
 * uses (its only includer), backed by the stb_image copy vendored with Tracy:
 *  - ilLoadL decodes PNG / JPG / BMP / TGA / GIF / PSD (8 or 16 bit per channel)
 *  - ilConvertImage converts between the luminance/alpha/RGB(A)/BGR(A) layouts
 *    and the UNSIGNED_BYTE / UNSIGNED_SHORT / FLOAT component types
 *  - saving (ilTexImage / ilSaveL / ilSaveImage) is not supported and fails
 * Headless never decodes DDS (CBitmap allocates a dummy), so DDS is not needed.
 *
 * One image is current at a time; CBitmap only touches DevIL under the texture
 * memory pool mutex, so no further locking is done here.
 *
 * Constant values match DevIL 1.8.0.
 */
#ifndef WASM_STUB_DEVIL_IL_H
#define WASM_STUB_DEVIL_IL_H

#include <cstdint>
#include <cstring>
#include <vector>

typedef unsigned int   ILenum;
typedef unsigned char  ILboolean;
typedef unsigned int   ILbitfield;
typedef signed char    ILbyte;
typedef double         ILclampd;
typedef float          ILclampf;
typedef double         ILdouble;
typedef float          ILfloat;
typedef int            ILint;
typedef short          ILshort;
typedef int            ILsizei;
typedef unsigned char  ILubyte;
typedef unsigned int   ILuint;
typedef unsigned short ILushort;
typedef void           ILvoid;
typedef char           ILchar;
typedef char*          ILstring;
typedef const char*    ILconst_string;

#define IL_FALSE 0
#define IL_TRUE  1

#define IL_VERSION 180

// formats
#define IL_COLOUR_INDEX     0x1900
#define IL_COLOR_INDEX      0x1900
#define IL_ALPHA            0x1906
#define IL_RGB              0x1907
#define IL_RGBA             0x1908
#define IL_BGR              0x80E0
#define IL_BGRA             0x80E1
#define IL_LUMINANCE        0x1909
#define IL_LUMINANCE_ALPHA  0x190A

// types
#define IL_BYTE             0x1400
#define IL_UNSIGNED_BYTE    0x1401
#define IL_SHORT            0x1402
#define IL_UNSIGNED_SHORT   0x1403
#define IL_INT              0x1404
#define IL_UNSIGNED_INT     0x1405
#define IL_FLOAT            0x1406
#define IL_DOUBLE           0x140A
#define IL_HALF             0x140B

// palette types
#define IL_PAL_NONE         0x0400
#define IL_PAL_RGB24        0x0401
#define IL_PAL_RGB32        0x0402
#define IL_PAL_RGBA32       0x0403
#define IL_PAL_BGR24        0x0404
#define IL_PAL_BGR32        0x0405
#define IL_PAL_BGRA32       0x0406

// file types
#define IL_TYPE_UNKNOWN     0x0000
#define IL_BMP              0x0420
#define IL_JPG              0x0425
#define IL_PNG              0x042A
#define IL_PNM              0x042B
#define IL_TGA              0x042D
#define IL_TIF              0x042E
#define IL_RAW              0x0430
#define IL_DDS              0x0437
#define IL_HDR              0x043F
#define IL_EXR              0x0442

// errors
#define IL_NO_ERROR             0x0000
#define IL_INVALID_ENUM         0x0501
#define IL_OUT_OF_MEMORY        0x0502
#define IL_FORMAT_NOT_SUPPORTED 0x0503
#define IL_INTERNAL_ERROR       0x0504
#define IL_INVALID_VALUE        0x0505
#define IL_ILLEGAL_OPERATION    0x0506
#define IL_COULD_NOT_OPEN_FILE  0x050A

// origin
#define IL_ORIGIN_SET         0x0600
#define IL_ORIGIN_LOWER_LEFT  0x0601
#define IL_ORIGIN_UPPER_LEFT  0x0602
#define IL_ORIGIN_MODE        0x0603

// hints / settings
#define IL_COMPRESSION_HINT   0x0668
#define IL_NO_COMPRESSION     0x0709
#define IL_JPG_QUALITY        0x0711

// image queries
#define IL_IMAGE_WIDTH            0x0DE4
#define IL_IMAGE_HEIGHT           0x0DE5
#define IL_IMAGE_DEPTH            0x0DE6
#define IL_IMAGE_SIZE_OF_DATA     0x0DE7
#define IL_IMAGE_BPP              0x0DE8
#define IL_IMAGE_BYTES_PER_PIXEL  0x0DE8
#define IL_IMAGE_BITS_PER_PIXEL   0x0DE9
#define IL_IMAGE_FORMAT           0x0DEA
#define IL_IMAGE_TYPE             0x0DEB
#define IL_PALETTE_TYPE           0x0DEC


// decoder: stb_image with internal linkage (this header has a single includer)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#if defined(__clang__)
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "../../../tracy/profiler/src/stb_image.h"
#if defined(__clang__)
	#pragma clang diagnostic pop
#endif


namespace wasm_il {
	struct Image {
		std::vector<uint8_t> data;
		ILuint width = 0;
		ILuint height = 0;
		ILenum format = IL_RGBA;
		ILenum type = IL_UNSIGNED_BYTE;
	};

	static Image curImage;
	static ILenum lastError = IL_NO_ERROR;
	static ILenum originMode = IL_ORIGIN_UPPER_LEFT;
	static bool originSet = false;

	static inline int FormatChannels(ILenum f) {
		switch (f) {
			case IL_ALPHA:           [[fallthrough]];
			case IL_LUMINANCE:       return 1;
			case IL_LUMINANCE_ALPHA: return 2;
			case IL_RGB:             [[fallthrough]];
			case IL_BGR:             return 3;
			case IL_RGBA:            [[fallthrough]];
			case IL_BGRA:            return 4;
			default:                 return 0;
		}
	}
	static inline int TypeSize(ILenum t) {
		switch (t) {
			case IL_BYTE:           [[fallthrough]];
			case IL_UNSIGNED_BYTE:  return 1;
			case IL_SHORT:          [[fallthrough]];
			case IL_UNSIGNED_SHORT: return 2;
			case IL_FLOAT:          return 4;
			default:                return 0;
		}
	}

	// component <-> normalized float
	static inline float ReadComp(const uint8_t* p, ILenum t) {
		switch (t) {
			case IL_UNSIGNED_BYTE:  return (*p) / 255.0f;
			case IL_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, 2); return v / 65535.0f; }
			case IL_FLOAT:          { float v; std::memcpy(&v, p, 4); return v; }
			default:                return 0.0f;
		}
	}
	static inline void WriteComp(uint8_t* p, ILenum t, float v) {
		v = (v < 0.0f)? 0.0f: ((v > 1.0f && t != IL_FLOAT)? 1.0f: v);
		switch (t) {
			case IL_UNSIGNED_BYTE:  { *p = static_cast<uint8_t>(v * 255.0f + 0.5f); } break;
			case IL_UNSIGNED_SHORT: { const uint16_t u = static_cast<uint16_t>(v * 65535.0f + 0.5f); std::memcpy(p, &u, 2); } break;
			case IL_FLOAT:          { std::memcpy(p, &v, 4); } break;
			default: break;
		}
	}

	// pixel <-> RGBA, honoring the BGR(A) swizzle; DevIL's luminance weights
	static inline void ToRGBA(const float* c, ILenum f, float rgba[4]) {
		switch (f) {
			case IL_ALPHA:           { rgba[0] = rgba[1] = rgba[2] = 1.0f; rgba[3] = c[0]; } break;
			case IL_LUMINANCE:       { rgba[0] = rgba[1] = rgba[2] = c[0]; rgba[3] = 1.0f; } break;
			case IL_LUMINANCE_ALPHA: { rgba[0] = rgba[1] = rgba[2] = c[0]; rgba[3] = c[1]; } break;
			case IL_RGB:             { rgba[0] = c[0]; rgba[1] = c[1]; rgba[2] = c[2]; rgba[3] = 1.0f; } break;
			case IL_BGR:             { rgba[0] = c[2]; rgba[1] = c[1]; rgba[2] = c[0]; rgba[3] = 1.0f; } break;
			case IL_RGBA:            { rgba[0] = c[0]; rgba[1] = c[1]; rgba[2] = c[2]; rgba[3] = c[3]; } break;
			case IL_BGRA:            { rgba[0] = c[2]; rgba[1] = c[1]; rgba[2] = c[0]; rgba[3] = c[3]; } break;
			default: break;
		}
	}
	static inline void FromRGBA(const float rgba[4], ILenum f, float* c) {
		const float lum = rgba[0] * 0.212671f + rgba[1] * 0.715160f + rgba[2] * 0.072169f;
		switch (f) {
			case IL_ALPHA:           { c[0] = rgba[3]; } break;
			case IL_LUMINANCE:       { c[0] = lum; } break;
			case IL_LUMINANCE_ALPHA: { c[0] = lum; c[1] = rgba[3]; } break;
			case IL_RGB:             { c[0] = rgba[0]; c[1] = rgba[1]; c[2] = rgba[2]; } break;
			case IL_BGR:             { c[0] = rgba[2]; c[1] = rgba[1]; c[2] = rgba[0]; } break;
			case IL_RGBA:            { c[0] = rgba[0]; c[1] = rgba[1]; c[2] = rgba[2]; c[3] = rgba[3]; } break;
			case IL_BGRA:            { c[0] = rgba[2]; c[1] = rgba[1]; c[2] = rgba[0]; c[3] = rgba[3]; } break;
			default: break;
		}
	}

	static inline void Reset() { curImage = Image{}; }
}


static inline void      ilInit(void) {}
static inline void      ilShutDown(void) { wasm_il::Reset(); }
static inline ILenum    ilGetError(void) { const ILenum e = wasm_il::lastError; wasm_il::lastError = IL_NO_ERROR; return e; }
static inline void      ilSetInteger(ILenum, ILint) {}
static inline ILubyte*  ilGetData(void) { return wasm_il::curImage.data.empty()? nullptr: wasm_il::curImage.data.data(); }
static inline ILboolean ilEnable(ILenum mode) { if (mode == IL_ORIGIN_SET) wasm_il::originSet = true; return IL_TRUE; }
static inline ILboolean ilDisable(ILenum mode) { if (mode == IL_ORIGIN_SET) wasm_il::originSet = false; return IL_TRUE; }
static inline ILboolean ilOriginFunc(ILenum mode) { wasm_il::originMode = mode; return IL_TRUE; }
static inline void      ilHint(ILenum, ILenum) {}
static inline void      ilGenImages(ILsizei num, ILuint* images) { for (ILsizei i = 0; i < num; ++i) images[i] = (ILuint)(i + 1); }
static inline void      ilDeleteImages(ILsizei, const ILuint*) { wasm_il::Reset(); }
static inline void      ilBindImage(ILuint) { wasm_il::Reset(); }

static inline ILint ilGetInteger(ILenum mode)
{
	const wasm_il::Image& img = wasm_il::curImage;
	const int bpp = wasm_il::FormatChannels(img.format) * wasm_il::TypeSize(img.type);

	switch (mode) {
		case IL_IMAGE_WIDTH:           return static_cast<ILint>(img.width);
		case IL_IMAGE_HEIGHT:          return static_cast<ILint>(img.height);
		case IL_IMAGE_DEPTH:           return 1;
		case IL_IMAGE_FORMAT:          return static_cast<ILint>(img.format);
		case IL_IMAGE_TYPE:            return static_cast<ILint>(img.type);
		case IL_IMAGE_BYTES_PER_PIXEL: return bpp;
		case IL_IMAGE_BITS_PER_PIXEL:  return bpp * 8;
		case IL_IMAGE_SIZE_OF_DATA:    return static_cast<ILint>(img.data.size());
		case IL_PALETTE_TYPE:          return IL_PAL_NONE;
		case IL_ORIGIN_MODE:           return static_cast<ILint>(wasm_il::originMode);
		default:                       return 0;
	}
}

static inline ILboolean ilLoadL(ILenum, const void* lump, ILuint size)
{
	using namespace wasm_il;
	Reset();

	const auto* buf = static_cast<const stbi_uc*>(lump);
	const int len = static_cast<int>(size);
	const bool is16 = (stbi_is_16_bit_from_memory(buf, len) != 0);

	int w = 0, h = 0, n = 0;
	void* pixels = is16?
		static_cast<void*>(stbi_load_16_from_memory(buf, len, &w, &h, &n, 0)):
		static_cast<void*>(stbi_load_from_memory(buf, len, &w, &h, &n, 0));

	if (pixels == nullptr) {
		lastError = IL_FORMAT_NOT_SUPPORTED;
		return IL_FALSE;
	}

	static constexpr ILenum formats[] = {0, IL_LUMINANCE, IL_LUMINANCE_ALPHA, IL_RGB, IL_RGBA};
	curImage.width = static_cast<ILuint>(w);
	curImage.height = static_cast<ILuint>(h);
	curImage.format = formats[n];
	curImage.type = is16? IL_UNSIGNED_SHORT: IL_UNSIGNED_BYTE;

	const size_t rowSize = size_t(w) * n * (is16? 2: 1);
	curImage.data.resize(rowSize * h);

	// stb decodes top row first (DevIL's IL_ORIGIN_UPPER_LEFT)
	const bool flip = (originSet && originMode == IL_ORIGIN_LOWER_LEFT);
	for (int y = 0; y < h; ++y) {
		const int srcRow = flip? (h - 1 - y): y;
		std::memcpy(curImage.data.data() + rowSize * y, static_cast<const uint8_t*>(pixels) + rowSize * srcRow, rowSize);
	}

	stbi_image_free(pixels);
	return IL_TRUE;
}

static inline ILboolean ilLoad(ILenum, ILconst_string) { wasm_il::lastError = IL_COULD_NOT_OPEN_FILE; return IL_FALSE; }

static inline ILboolean ilConvertImage(ILenum dstFormat, ILenum dstType)
{
	using namespace wasm_il;
	Image& img = curImage;

	if (img.data.empty()) {
		lastError = IL_ILLEGAL_OPERATION;
		return IL_FALSE;
	}
	if (dstFormat == img.format && dstType == img.type)
		return IL_TRUE;

	const int srcCh = FormatChannels(img.format), srcSz = TypeSize(img.type);
	const int dstCh = FormatChannels(dstFormat), dstSz = TypeSize(dstType);

	if (dstCh == 0 || dstSz == 0 || srcCh == 0 || srcSz == 0) {
		lastError = IL_INVALID_ENUM;
		return IL_FALSE;
	}

	const size_t numPixels = size_t(img.width) * img.height;
	std::vector<uint8_t> out(numPixels * dstCh * dstSz);

	for (size_t i = 0; i < numPixels; ++i) {
		float src[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		float dst[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		float rgba[4];

		for (int c = 0; c < srcCh; ++c)
			src[c] = ReadComp(img.data.data() + (i * srcCh + c) * srcSz, img.type);

		ToRGBA(src, img.format, rgba);
		FromRGBA(rgba, dstFormat, dst);

		for (int c = 0; c < dstCh; ++c)
			WriteComp(out.data() + (i * dstCh + c) * dstSz, dstType, dst[c]);
	}

	img.data = std::move(out);
	img.format = dstFormat;
	img.type = dstType;
	return IL_TRUE;
}

// saving is not supported on WASM (screenshots and image dumps only)
static inline ILboolean ilTexImage(ILuint, ILuint, ILuint, ILubyte, ILenum, ILenum, void*) { wasm_il::lastError = IL_FORMAT_NOT_SUPPORTED; return IL_FALSE; }
static inline ILuint    ilSaveL(ILenum, void*, ILuint) { wasm_il::lastError = IL_FORMAT_NOT_SUPPORTED; return 0; }
static inline ILboolean ilSaveImage(ILconst_string) { wasm_il::lastError = IL_FORMAT_NOT_SUPPORTED; return IL_FALSE; }

#endif // WASM_STUB_DEVIL_IL_H
