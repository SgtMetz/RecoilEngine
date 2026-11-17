# WASM browser build with the real renderer (phase 2, B2): spring-web.{js,wasm}
# Same engine sources as builds/legacy, compiled without HEADLESS, linked against the
# Emscripten SDL2 port and WebGL2 (through glad + Rendering/GL/WebGL, the desktop-GL shim).
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

add_definitions(-DRECOIL_WEB_RENDER)
remove_definitions(-DNO_SOUND) # the root CMakeLists defines it for the headless-style default; this target has real sound
remove_definitions(-DAVI_CAPTURING)

find_package(SDL2 MODULE REQUIRED)

if (NOT TARGET prd-wasm-stub)
	add_library(prd-wasm-stub STATIC "${ENGINE_SRC_ROOT_DIR}/lib/wasm-stubs/prdownloader/prd_stub.cpp")
	target_link_libraries(prd-wasm-stub PRIVATE pr-md5 prd::base64)
endif ()

list(APPEND engineWebLibraries SDL2::SDL2)
list(APPEND engineWebLibraries glad)
list(APPEND engineWebLibraries Freetype::Freetype)
list(APPEND engineWebLibraries squish rgetc1)
list(APPEND engineWebLibraries web-sound)
list(APPEND engineWebLibraries engineSystemNet)
list(APPEND engineWebLibraries ${engineCommonLibraries})
list(APPEND engineWebLibraries ${SPRING_SIM_LIBRARIES})
list(APPEND engineWebLibraries engineSim)
list(APPEND engineWebLibraries prd-wasm-stub)
list(APPEND engineWebLibraries RmlUi::Core)
list(APPEND engineWebLibraries RmlUi::Debugger)
list(APPEND engineWebLibraries lunasvg)

include_directories(${ENGINE_SRC_ROOT_DIR}/lib/assimp/include)
include_directories(${ENGINE_SRC_ROOT_DIR}/lib/wasm-stubs/openal ${EMSCRIPTEN_SYSROOT}/include/AL) # al.h, alc.h, efx.h for the sound headers the game code includes
include_directories(${ENGINE_SRC_ROOT_DIR}/lib/asio/include)
include_directories(${ENGINE_SRC_ROOT_DIR}/lib/slimsig/include)
include_directories(${ENGINE_SRC_ROOT_DIR}/lib/cereal/include)


# Audio (B3): the engine's OpenAL sound code on Emscripten's OpenAL (WebAudio, proxied to the
# page's main thread) and the Ogg/Vorbis ports. Emscripten's OpenAL has no EFX: lib/wasm-stubs/openal/efx.h
# only declares it, and the engine finds ALC_EXT_EFX absent at run time. Only engine-web uses this;
# the headless targets stay on no-sound.
set(webSoundDir "${ENGINE_SRC_ROOT_DIR}/System/Sound")
add_library(web-sound STATIC
	${webSoundDir}/ISound.cpp
	${webSoundDir}/IAudioChannel.cpp
	${webSoundDir}/Null/NullSound.cpp
	${webSoundDir}/OpenAL/ALShared.cpp
	${webSoundDir}/OpenAL/EFX.cpp
	${webSoundDir}/OpenAL/EFXfuncs.cpp
	${webSoundDir}/OpenAL/EFXPresets.cpp
	${webSoundDir}/OpenAL/AudioChannel.cpp
	${webSoundDir}/OpenAL/OggDecoder.cpp
	${webSoundDir}/OpenAL/Mp3Decoder.cpp
	${webSoundDir}/OpenAL/MusicStream.cpp
	${webSoundDir}/OpenAL/Sound.cpp
	${webSoundDir}/OpenAL/SoundChannels.cpp
	${webSoundDir}/OpenAL/SoundBuffer.cpp
	${webSoundDir}/OpenAL/SoundItem.cpp
	${webSoundDir}/OpenAL/SoundSource.cpp
	${webSoundDir}/OpenAL/VorbisShared.cpp
)
target_include_directories(web-sound PRIVATE
	"${ENGINE_SRC_ROOT_DIR}/lib/wasm-stubs/openal"
	"${EMSCRIPTEN_SYSROOT}/include/AL")
target_compile_options(web-sound PRIVATE -sUSE_OGG=1 -sUSE_VORBIS=1)
target_link_libraries(web-sound SDL2::SDL2 Tracy::TracyClient nowide::nowide)

add_library(GameWeb STATIC)
target_sources(GameWeb PRIVATE ${sources_engine_Game})
target_include_directories(GameWeb PRIVATE "${CMAKE_BINARY_DIR}/src-generated/engine")
add_dependencies(GameWeb generateVersionFiles)
target_link_libraries(GameWeb PRIVATE
	Tracy::TracyClient
	SDL2::SDL2
	glad
	RmlUi::Core
	web-sound
	prd::jsoncpp
	streflop
	nowide::nowide
	fmt::fmt
)

file(GLOB sources_engine_WebGL "${ENGINE_SRC_ROOT_DIR}/Rendering/GL/WebGL/*.cpp")

# typed no-op stubs for every glad entry point (WebGL2 lacks most of desktop GL)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(webglStubsInc "${CMAKE_BINARY_DIR}/src-generated/engine/WebGLStubs.gen.inc")
add_custom_command(
	OUTPUT "${webglStubsInc}"
	COMMAND ${Python3_EXECUTABLE} "${ENGINE_SRC_ROOT_DIR}/Rendering/GL/WebGL/gen_stubs.py"
		"${ENGINE_SRC_ROOT_DIR}/lib/glad/glad.h" "${webglStubsInc}"
	DEPENDS "${ENGINE_SRC_ROOT_DIR}/Rendering/GL/WebGL/gen_stubs.py" "${ENGINE_SRC_ROOT_DIR}/lib/glad/glad.h"
	VERBATIM
)

add_executable(engine-web
	${engineSources}
	${sources_engine_WebGL}
	"${webglStubsInc}"
	"${ENGINE_SRC_ROOT_DIR}/System/Platform/Emscripten/WebFS.cpp"
)
target_link_libraries(engine-web web-sound ${engineWebLibraries} GameWeb web-sound)
add_dependencies(engine-web generateVersionFiles basecontent)

# Optional: glslang + SPIRV-Cross (built for Emscripten, found through CMAKE_PREFIX_PATH)
# compile the engine's desktop GLSL to SPIR-V and back to GLSL ES 3.00, which handles what
# the text translator cannot (implicit conversions, GLSL 4 syntax). Without them only the
# text translation is used.
find_package(glslang CONFIG QUIET NO_CMAKE_FIND_ROOT_PATH)
find_package(spirv_cross_core CONFIG QUIET NO_CMAKE_FIND_ROOT_PATH)
find_package(spirv_cross_glsl CONFIG QUIET NO_CMAKE_FIND_ROOT_PATH)
if (glslang_FOUND AND spirv_cross_core_FOUND AND spirv_cross_glsl_FOUND)
	message(STATUS "engine-web: shader cross-compilation with glslang ${glslang_VERSION} + SPIRV-Cross")
	target_compile_definitions(engine-web PRIVATE RECOIL_WEB_GLSLANG)
	target_link_libraries(engine-web glslang::glslang glslang::glslang-default-resource-limits spirv-cross-glsl spirv-cross-core)
else ()
	message(STATUS "engine-web: glslang/SPIRV-Cross not found, text shader translation only")
endif ()

# OffscreenCanvas: with PROXY_TO_PTHREAD the engine's main thread is a worker; the page's
# <canvas id="canvas"> is transferred to it and the WebGL2 context lives there.
target_link_options(engine-web PRIVATE
	${RECOIL_WASM_WEB_LINK_OPTIONS}
	-sUSE_SDL=2
	-sUSE_OGG=1
	-sUSE_VORBIS=1
	-lopenal
	-sMIN_WEBGL_VERSION=2
	-sMAX_WEBGL_VERSION=2
	-sFULL_ES3=1
	-sOFFSCREENCANVAS_SUPPORT=1
	"-sOFFSCREENCANVASES_TO_PTHREAD=#canvas"
	# spring-web.js.symbols: wasm function index -> name, for reading stack traces
	--emit-symbol-map
)
set_target_properties(engine-web PROPERTIES
	OUTPUT_NAME "spring-web"
	LINK_DEPENDS "${ENGINE_SRC_ROOT_DIR}/lib/wasm-stubs/web-pre.js"
)
