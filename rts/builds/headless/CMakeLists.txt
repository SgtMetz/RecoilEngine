# Place executables and shared libs under "build-dir/",
# instead of under "build-dir/rts/"
# This way, we have the build-dir structure more like the install-dir one,
# which makes testing spring in the builddir easier, eg. like this:
# cd build-dir
# SPRING_DATADIR=$(pwd) ./spring
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

add_definitions(-DHEADLESS)
add_definitions(-DNO_SOUND)
remove_definitions(-DAVI_CAPTURING)

set(OpenGL_GL_PREFERENCE LEGACY)

# Headless links headlessStubs instead of libGL; only the GL headers are needed.
# WASM: Emscripten ships GL headers in its sysroot, so the system OpenGL check is skipped.
if (NOT IS_WASM_BUILD)
	find_package(OpenGL 3.0 REQUIRED)
endif ()

# NOTE(create_headless_target): We don't need to copy COMPILE_DEFINTIONS till the add_definitions are used.
# We don't need to copy COMPILE_DEFINTIONS because the headless defines are defined at the folder level
# and we don't want to inherit original ones
function(create_headless_target_from targetName)
	get_target_property(targetIncludes ${targetName} INCLUDE_DIRECTORIES)
	get_target_property(targetSources ${targetName} SOURCES)

	set(newTargetName "${targetName}Headless")
	add_library(${newTargetName} STATIC)
	if (targetIncludes)
		target_include_directories(${newTargetName} PRIVATE ${targetIncludes})
	endif()
	target_sources(${newTargetName} PRIVATE ${targetSources})
endfunction()


# headlessstubs are our stubs that replace libGL, libGLU, libGLEW, libSDL (yes really!)
list(APPEND engineHeadlessLibraries headlessStubs)
list(APPEND engineHeadlessLibraries ${SPRING_SIM_LIBRARIES})
list(APPEND engineHeadlessLibraries engineSystemNet)
list(APPEND engineHeadlessLibraries ${engineCommonLibraries})
#list(APPEND engineHeadlessLibraries engineaGui)
list(APPEND engineHeadlessLibraries no-sound)
list(APPEND engineHeadlessLibraries engineSim)
list(APPEND engineHeadlessLibraries squish)
if (NOT IS_WASM_BUILD)
	list(APPEND engineHeadlessLibraries pr-downloader)
else ()
	# WASM: no libcurl; content is preloaded into the VFS. Every download request fails cleanly.
	add_library(prd-wasm-stub STATIC "${ENGINE_SRC_ROOT_DIR}/lib/wasm-stubs/prdownloader/prd_stub.cpp")
	target_link_libraries(prd-wasm-stub PRIVATE pr-md5 prd::base64)
	list(APPEND engineHeadlessLibraries prd-wasm-stub)
endif ()
list(APPEND engineHeadlessLibraries RmlUi::Core)
list(APPEND engineHeadlessLibraries RmlUi::Debugger)
list(APPEND engineHeadlessLibraries lunasvg)

include_directories(${ENGINE_SRC_ROOT_DIR}/lib/assimp/include)
include_directories(${ENGINE_SRC_ROOT_DIR}/lib/asio/include)
include_directories(${ENGINE_SRC_ROOT_DIR}/lib/slimsig/include)
include_directories(${ENGINE_SRC_ROOT_DIR}/lib/cereal/include)

if (TARGET Game)
	create_headless_target_from(Game)
else ()
	# BUILD_spring-legacy=OFF (e.g. WASM): the Game target from builds/legacy does not exist,
	# so assemble GameHeadless from the same source list directly.
	add_library(GameHeadless STATIC)
	target_sources(GameHeadless PRIVATE ${sources_engine_Game})
	target_include_directories(GameHeadless PRIVATE "${CMAKE_BINARY_DIR}/src-generated/engine")
	add_dependencies(GameHeadless generateVersionFiles)
endif ()
target_link_libraries(GameHeadless PRIVATE
	Tracy::TracyClient
	headlessStubs
	RmlUi::Core
	prd::jsoncpp
	streflop
	nowide::nowide
	fmt::fmt
)
### Build the executable
add_executable(engine-headless ${engineSources} ${ENGINE_ICON})
target_link_libraries(engine-headless no-sound ${engineHeadlessLibraries} GameHeadless no-sound)

# Export symbols for plugin access (replaces CMP0065 OLD behavior)
set_target_properties(engine-headless PROPERTIES ENABLE_EXPORTS TRUE)

if (IS_WASM_BUILD)
	# see RECOIL_WASM_NODE_LINK_OPTIONS in the top-level CMakeLists.txt
	target_link_options(engine-headless PRIVATE ${RECOIL_WASM_NODE_LINK_OPTIONS})
endif ()

if(MSVC)
    target_link_libraries(engine-headless engine-natvis)
endif()

if    (MINGW)
	# To enable console output/force a console window to open
	set_target_properties(engine-headless PROPERTIES LINK_FLAGS "-Wl,-subsystem,console")
endif (MINGW)


### Install the executable
install(TARGETS engine-headless DESTINATION ${BINDIR})

# Only build & install spring-headless executable & dependencies
# use cases:
# * make spring-headless
# * make install-spring-headless
create_engine_build_and_install_target(headless)
