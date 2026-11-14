/* WASM port placeholder for DevIL's <IL/il.h>.
 *
 * Used only when building for Emscripten without a real DevIL (WASM_NO_DEVIL).
 * Declares just the subset of DevIL that rts/Rendering/Textures/Bitmap.cpp uses.
 * Every load/save/convert reports failure, so CBitmap behaves as if the image
 * could not be decoded. Replace with a real DevIL build (CMAKE_PREFIX_PATH=~/wasm-deps)
 * before relying on image content.
 *
 * Constant values match DevIL 1.8.0.
 */
#ifndef WASM_STUB_DEVIL_IL_H
#define WASM_STUB_DEVIL_IL_H

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

#ifdef __cplusplus
extern "C++" {
#endif

static inline void      ilInit(void) {}
static inline void      ilShutDown(void) {}
static inline ILenum    ilGetError(void) { return IL_NO_ERROR; }
static inline ILint     ilGetInteger(ILenum) { return 0; }
static inline void      ilSetInteger(ILenum, ILint) {}
static inline ILubyte*  ilGetData(void) { return 0; }
static inline ILboolean ilEnable(ILenum) { return IL_TRUE; }
static inline ILboolean ilDisable(ILenum) { return IL_TRUE; }
static inline ILboolean ilOriginFunc(ILenum) { return IL_TRUE; }
static inline void      ilHint(ILenum, ILenum) {}
static inline void      ilGenImages(ILsizei num, ILuint* images) { for (ILsizei i = 0; i < num; ++i) images[i] = (ILuint)(i + 1); }
static inline void      ilDeleteImages(ILsizei, const ILuint*) {}
static inline void      ilBindImage(ILuint) {}
static inline ILboolean ilLoad(ILenum, ILconst_string) { return IL_FALSE; }
static inline ILboolean ilLoadL(ILenum, const void*, ILuint) { return IL_FALSE; }
static inline ILuint    ilSaveL(ILenum, void*, ILuint) { return 0; }
static inline ILboolean ilSaveImage(ILconst_string) { return IL_FALSE; }
static inline ILboolean ilTexImage(ILuint, ILuint, ILuint, ILubyte, ILenum, ILenum, void*) { return IL_FALSE; }
static inline ILboolean ilConvertImage(ILenum, ILenum) { return IL_FALSE; }

#ifdef __cplusplus
}
#endif

#endif // WASM_STUB_DEVIL_IL_H
