cmake_minimum_required(VERSION 3.24)

# Supply the official toolchain on the FIRST configure; never set compilers here.
if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake")
endif()
project(gtavcs_nx VERSION 0.4.0 LANGUAGES C CXX)

if(NOT NINTENDO_SWITCH OR NOT COMMAND nx_create_nro)
    message(FATAL_ERROR "The official devkitPro Switch toolchain and switch-cmake package are required")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# Inputs, all explicit (tools/build_game.py passes them): the overlaid pinned upstream checkout, the locked public
# dependencies and - for the game NRO only - the private generated EE/VU archive built from the user's disc.
set(VCS_UPSTREAM_DIR "" CACHE PATH "Overlaid pinned upstream checkout (tools/upstream_overlay.py)")
set(SSE2NEON_INCLUDE_DIR "" CACHE PATH "Locked sse2neon (tools/core-dependencies.json)")
set(NX_IMGUI_DIR "" CACHE PATH "Locked Dear ImGui (tools/core-dependencies.json)")
set(NX_PRIVATE_GENERATED_ARCHIVE "" CACHE FILEPATH "Private EE/VU archive; never used by source CI")
foreach(required VCS_UPSTREAM_DIR SSE2NEON_INCLUDE_DIR NX_IMGUI_DIR)
    if(NOT IS_DIRECTORY "${${required}}")
        message(FATAL_ERROR "Provide ${required} explicitly (tools/build_game.py)")
    endif()
endforeach()
file(READ "${PORT_ROOT}/tools/core-dependencies.json" dependency_lock)
string(JSON imgui_header GET "${dependency_lock}" imgui header_sha256)
file(SHA256 "${NX_IMGUI_DIR}/imgui.h" imgui_actual)
if(NOT imgui_actual STREQUAL imgui_header)
    message(FATAL_ERROR "Dear ImGui content does not match the locked dependency")
endif()
file(READ "${PORT_ROOT}/patches/upstream/series.json" source_lock)
string(JSON expected_commit GET "${source_lock}" base_commit)
include("${PORT_ROOT}/cmake/CPUOptions.cmake")

if(NOT DEVKITPRO)
    set(DEVKITPRO "$ENV{DEVKITPRO}")
endif()
set(SWITCH_PORTLIBS "${DEVKITPRO}/portlibs/switch")
# Never ask Ubuntu's pkg-config for host SDL, GL, X11 or Wayland dependencies.
find_program(PKG_CONFIG_EXECUTABLE NAMES aarch64-none-elf-pkg-config
    PATHS "${SWITCH_PORTLIBS}/bin" NO_DEFAULT_PATH REQUIRED)
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${SWITCH_PORTLIBS}/lib/pkgconfig:${SWITCH_PORTLIBS}/share/pkgconfig")
find_package(PkgConfig REQUIRED)
pkg_check_modules(NX_SDL2 REQUIRED sdl2>=2.0.7)
# Switch Mesa exposes desktop entry points through EGL + glapi; no gl.pc.
pkg_check_modules(NX_GL REQUIRED glapi)
pkg_check_modules(NX_EGL REQUIRED egl)
pkg_check_modules(NX_ZLIB REQUIRED zlib)
# Movies (M07-06): the locked image's switch-ffmpeg 7.1 (a static target build, --enable-gpl, docs/milestones/M01/
# package-lock.txt), the 7.1 line upstream's desktop build pins; only the three libraries the movie player and the
# runtime's MPEG decoder include.
pkg_check_modules(NX_FFMPEG REQUIRED libavcodec>=61 libavutil>=59 libswscale>=8)
find_library(NX_LIBRARY NAMES nx PATHS "${DEVKITPRO}/libnx/lib" NO_DEFAULT_PATH REQUIRED)
execute_process(COMMAND dkp-pacman -Q libnx OUTPUT_VARIABLE NX_PACKAGE
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)

set(upstream "${VCS_UPSTREAM_DIR}")
set(runtime "${upstream}/third_party/PS2Recomp/ps2xRuntime")

add_library(glad STATIC third_party/glad/src/gl.c)
target_include_directories(glad PUBLIC third_party/glad/include)

# The OpenGL RHI's shaders (M05): GLSL 330 ports of upstream's HLSL (src/render/glsl), one string per variant upstream
# builds; tools/glsl_shaders.py reads the variant list from upstream's src/render/CMakeLists.txt.
find_program(NX_PYTHON NAMES python3 REQUIRED)
set(NX_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(GLOB nx_glsl_sources CONFIGURE_DEPENDS "${PORT_ROOT}/src/render/glsl/*.glsl")
add_custom_command(
    OUTPUT "${NX_SHADER_DIR}/glsl/gs_shaders_glsl.h" "${CMAKE_CURRENT_BINARY_DIR}/shader-manifest.json"
    COMMAND "${NX_PYTHON}" -I "${PORT_ROOT}/tools/glsl_shaders.py" --upstream "${upstream}"
            --out "${NX_SHADER_DIR}/glsl/gs_shaders_glsl.h" --manifest "${CMAKE_CURRENT_BINARY_DIR}/shader-manifest.json"
    DEPENDS ${nx_glsl_sources} "${PORT_ROOT}/tools/glsl_shaders.py" "${upstream}/src/render/CMakeLists.txt"
    COMMENT "GLSL shader variants"
    VERBATIM)
add_custom_target(nx_glsl DEPENDS "${NX_SHADER_DIR}/glsl/gs_shaders_glsl.h")

# Dear ImGui (upstream's pinned version) with its SDL2 input backend; the null RHI takes its draw data until M05.
add_library(nx_imgui STATIC "${NX_IMGUI_DIR}/imgui.cpp" "${NX_IMGUI_DIR}/imgui_draw.cpp"
    "${NX_IMGUI_DIR}/imgui_tables.cpp" "${NX_IMGUI_DIR}/imgui_widgets.cpp" "${NX_IMGUI_DIR}/imgui_demo.cpp"
    "${NX_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp")
target_include_directories(nx_imgui PUBLIC "${NX_IMGUI_DIR}" "${NX_IMGUI_DIR}/backends"
    PRIVATE ${NX_SDL2_INCLUDE_DIRS})
target_compile_options(nx_imgui PRIVATE -O2 ${NX_SDL2_CFLAGS_OTHER})
# no shell on the Switch (the default opens URLs with fork / exec)
target_compile_definitions(nx_imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

# Upstream's runtime library, the source list of its ps2xRuntime/CMakeLists.txt (the debug panel, paraLLEl-GS,
# FFmpeg and the desktop runner main left out). Not headless: the game's UI and perf overlay use Dear ImGui.
set(runtime_sources
    game_overrides.cpp gs/ps2_gif_arbiter.cpp ps2_audio.cpp ps2_audio_vag.cpp
    gs/ps2_gs_memory.cpp gs/gs_frontend.cpp gs/gs_cpu_backend.cpp
    ps2_iop_host.cpp ps2_memory.cpp ps2_pad.cpp ps2_rom_device.cpp ps2_vfs.cpp
    ps2_disc_image.cpp ps2_audio_out.cpp ps2_perf.cpp ps2_control.cpp
    ps2_hle_survey.cpp ps2_runtime.cpp ps2_vif1_interpreter.cpp
    ps2_render_worker.cpp ps2_write_watch.cpp ps2_host_std.cpp ps2_iop_native.cpp
    vu/ps2_vu1_core.cpp vu/ps2_vu1_upper.cpp vu/ps2_vu1_lower.cpp
    vu/ps2_vu_code_log.cpp games_database.cpp
    gs/gs_gpu.cpp gs/gs_hw.cpp gs/gs_gpu_none.cpp)
list(TRANSFORM runtime_sources PREPEND "${runtime}/src/lib/")
file(GLOB_RECURSE kernel_sources CONFIGURE_DEPENDS "${runtime}/src/lib/Kernel/*.cpp")
set(PS2X_IOP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${upstream}/third_party/PS2Recomp/ps2xIOP" iop)
nx_cpu_options(ps2_iop)

# The game layer: every upstream src/ source except the desktop platform (SDL3), the D3D11 / Vulkan backends and
# main(); main.cpp itself is built without main() for the launcher steps (src/launcher.h).
file(GLOB_RECURSE game_sources CONFIGURE_DEPENDS "${upstream}/src/*.cpp")
list(FILTER game_sources EXCLUDE REGEX "/src/(platform|rhi)/")
list(FILTER game_sources EXCLUDE REGEX "/src/render/")
# the desktop ISO reader (mmap); the Switch reads the extracted folder disc (src/vfs/folder_disc.cpp)
list(FILTER game_sources EXCLUDE REGEX "/src/disc/iso_image.cpp$")
# These include the generated function declarations (ps2_recompiled_functions.h, made from the user's ELF): built only
# with the private generated tree, in nx_game_private below.
set(private_game_sources "${upstream}/src/debug/selftest.cpp" "${upstream}/src/game/vcs_display.cpp"
    "${upstream}/src/game/vcs_overrides.cpp")
list(REMOVE_ITEM game_sources ${private_game_sources})
# Sources that only register themselves through a file-scope object (nothing calls into them): a plain archive
# member like that is dropped by the linker, so they get their own library, linked whole-archive below.
# vcs_vu0_native.cpp: its Registrar adds the native VU0 programs (ps2RegisterVu0Native).
set(self_registering_sources "${upstream}/src/game/vcs_vu0_native.cpp")
list(REMOVE_ITEM game_sources ${self_registering_sources})
add_library(nx_core STATIC ${runtime_sources} ${kernel_sources} ${game_sources}
    "${upstream}/src/render/presenter.cpp" "${upstream}/src/render/gs_shaders.cpp"
    "${upstream}/src/render/ui_renderer.cpp" "${upstream}/src/rhi/rhi.cpp" "${upstream}/src/rhi/null/rhi_null.cpp"
    src/rhi/gl/rhi_gl.cpp src/core/core.cpp src/core/run_config.cpp src/vfs/folder_disc.cpp src/vfs/game_paths.cpp
    src/switch/branch_trace.cpp src/devmenu/dev_menu.cpp)
nx_cpu_options(nx_core)
target_include_directories(nx_core PRIVATE "${upstream}/src" "${runtime}/src/lib" "${PORT_ROOT}/src" "${NX_SHADER_DIR}")
add_dependencies(nx_core nx_glsl)
# the OpenGL RHI (src/rhi/gl) is the Switch's renderer; the null backend stays selectable (VCS_RHI=null)
target_compile_definitions(nx_core PUBLIC VCS_HAVE_OPENGL=1)
# Movies (M07): vcs_movie.cpp keys them on __has_include and the runtime's MPEG decoder on PS2X_HAS_FFMPEG, so
# FFmpeg's include folder goes on with the switch (one value in every library that holds runtime or game code).
target_include_directories(nx_core PRIVATE ${NX_FFMPEG_INCLUDE_DIRS})
target_compile_definitions(nx_core PRIVATE PS2X_HAS_FFMPEG=1 PS2X_ENABLE_IOP_RPC_TRACE=0 _POSIX_C_SOURCE=200809L
    VCS_LAUNCHER_NO_MAIN)
target_link_libraries(nx_core PUBLIC ps2_iop nx_imgui nx_platform glad)

add_library(nx_self_registering STATIC ${self_registering_sources})
nx_cpu_options(nx_self_registering)
target_include_directories(nx_self_registering PRIVATE "${upstream}/src" "${runtime}/src/lib")
target_compile_definitions(nx_self_registering PRIVATE PS2X_HAS_FFMPEG=1 PS2X_ENABLE_IOP_RPC_TRACE=0
    _POSIX_C_SOURCE=200809L)
target_link_libraries(nx_self_registering PUBLIC nx_core)

# upstream's platform.h on SDL2 (src/platform/platform_switch_sdl2.cpp)
add_library(nx_platform STATIC src/platform/platform_switch_sdl2.cpp src/platform/gl_loader_egl.cpp src/devmenu/chord.cpp)
target_include_directories(nx_platform PRIVATE "${upstream}/src" "${PORT_ROOT}/src"
    ${NX_SDL2_INCLUDE_DIRS} ${NX_ZLIB_INCLUDE_DIRS} ${NX_EGL_INCLUDE_DIRS})
target_compile_options(nx_platform PRIVATE -O2 -fPIE ${NX_SDL2_CFLAGS_OTHER} ${NX_EGL_CFLAGS_OTHER})
target_link_libraries(nx_platform PUBLIC nx_imgui glad)

# The entry point and the libnx link wraps, compile-only: source CI builds this target too (the executable below needs
# the private archive), so main.c and src/switch/*.c are compiled on every change. The executable compiles the same
# sources with the same warning and floating-point options (not identical: PIC here, PIE there, and the executable
# inherits more include directories and definitions from the libraries it links).
set(nx_entry_sources src/main.c src/switch/output.c src/switch/thread_stack.c src/switch/clocks.c src/switch/sampler.c src/switch/open_audit.c)
add_library(nx_entry_check OBJECT ${nx_entry_sources})
target_include_directories(nx_entry_check PRIVATE src ${NX_SDL2_INCLUDE_DIRS} ${NX_GL_INCLUDE_DIRS} ${NX_EGL_INCLUDE_DIRS})
target_compile_definitions(nx_entry_check PRIVATE SDL_MAIN_HANDLED GTAVCS_LIBNX_PACKAGE="${NX_PACKAGE}")
target_compile_options(nx_entry_check PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-strict-aliasing
    ${NX_SDL2_CFLAGS_OTHER} ${NX_GL_CFLAGS_OTHER} ${NX_EGL_CFLAGS_OTHER})
target_link_libraries(nx_entry_check PRIVATE glad)

# The RHI conformance scenes on the Switch (src/rhi_tests, M05): every rhi::Device method and GLSL variant through the
# OpenGL backend on SDL2's context, readbacks to the SD card. Public: no game code, no assets.
add_library(nx_rhi_tests STATIC src/rhi_tests/rhi_conformance.cpp src/rhi/gl/rhi_gl.cpp
    "${upstream}/src/rhi/rhi.cpp" "${upstream}/src/rhi/null/rhi_null.cpp" "${upstream}/src/render/gs_shaders.cpp")
target_include_directories(nx_rhi_tests PUBLIC "${PORT_ROOT}/src" "${upstream}/src" "${NX_SHADER_DIR}")
target_compile_definitions(nx_rhi_tests PUBLIC VCS_HAVE_OPENGL=1)
target_compile_options(nx_rhi_tests PRIVATE -Wall -Wextra -ffp-contract=off)
target_link_libraries(nx_rhi_tests PUBLIC glad nx_imgui)
add_dependencies(nx_rhi_tests nx_glsl)
add_executable(gtavcs_rhi_tests src/rhi_tests/switch_main.cpp src/platform/gl_loader_egl.cpp)
set_target_properties(gtavcs_rhi_tests PROPERTIES SUFFIX ".elf")
target_include_directories(gtavcs_rhi_tests PRIVATE ${NX_SDL2_INCLUDE_DIRS} ${NX_EGL_INCLUDE_DIRS})
target_compile_definitions(gtavcs_rhi_tests PRIVATE SDL_MAIN_HANDLED _POSIX_C_SOURCE=200809L)
target_compile_options(gtavcs_rhi_tests PRIVATE -Wall -Wextra ${NX_SDL2_CFLAGS_OTHER} ${NX_EGL_CFLAGS_OTHER})
target_link_directories(gtavcs_rhi_tests PRIVATE "${SWITCH_PORTLIBS}/lib")
target_link_options(gtavcs_rhi_tests PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/gtavcs_rhi_tests.map"
    ${NX_SDL2_STATIC_LDFLAGS_OTHER} ${NX_GL_STATIC_LDFLAGS_OTHER} ${NX_EGL_STATIC_LDFLAGS_OTHER})
target_link_libraries(gtavcs_rhi_tests PRIVATE nx_rhi_tests
    -Wl,--start-group ${NX_SDL2_STATIC_LIBRARIES} ${NX_GL_STATIC_LIBRARIES} ${NX_EGL_STATIC_LIBRARIES} "${NX_LIBRARY}" m
    -Wl,--end-group)
nx_generate_nacp(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gtavcs_rhi_tests.nacp"
    NAME "GTA VCS NX RHI tests" AUTHOR "gtavcs_nx contributors" VERSION "${PROJECT_VERSION}")
nx_create_nro(gtavcs_rhi_tests OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gtavcs_rhi_tests.nro"
    NACP "${CMAKE_CURRENT_BINARY_DIR}/gtavcs_rhi_tests.nacp")

if(NOT NX_PRIVATE_GENERATED_ARCHIVE)
    # Source CI: every public piece compiles; without the user's generated code there is no game to link.
    message(STATUS "No private generated archive: building the public core library only (no NRO)")
    return()
endif()
set(NX_PRIVATE_GENERATED_DIR "" CACHE PATH "Private generated EE folder (ps2_recompiled_functions.h)")
if(NOT EXISTS "${NX_PRIVATE_GENERATED_ARCHIVE}" OR NOT EXISTS "${NX_PRIVATE_GENERATED_DIR}/ps2_recompiled_functions.h")
    message(FATAL_ERROR "NX_PRIVATE_GENERATED_ARCHIVE and NX_PRIVATE_GENERATED_DIR must name the private build")
endif()
add_library(nx_game_private STATIC ${private_game_sources})
nx_cpu_options(nx_game_private)
target_include_directories(nx_game_private PRIVATE "${upstream}/src" "${runtime}/src/lib" "${NX_PRIVATE_GENERATED_DIR}")
target_compile_definitions(nx_game_private PRIVATE PS2X_HAS_FFMPEG=1 PS2X_ENABLE_IOP_RPC_TRACE=0
    _POSIX_C_SOURCE=200809L)
target_link_libraries(nx_game_private PUBLIC nx_core)

add_executable(gtavcs ${nx_entry_sources})
set_target_properties(gtavcs PROPERTIES SUFFIX ".elf" LINKER_LANGUAGE CXX)
target_include_directories(gtavcs PRIVATE src
    ${NX_SDL2_INCLUDE_DIRS} ${NX_GL_INCLUDE_DIRS} ${NX_EGL_INCLUDE_DIRS})
target_compile_definitions(gtavcs PRIVATE SDL_MAIN_HANDLED GTAVCS_LIBNX_PACKAGE="${NX_PACKAGE}")
target_compile_options(gtavcs PRIVATE -Wall -Wextra -Wpedantic
    -ffp-contract=off -fno-strict-aliasing
    ${NX_SDL2_CFLAGS_OTHER} ${NX_GL_CFLAGS_OTHER} ${NX_EGL_CFLAGS_OTHER})
target_link_directories(gtavcs PRIVATE "${SWITCH_PORTLIBS}/lib")
target_link_options(gtavcs PRIVATE
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/gtavcs.map" -Wl,--wrap=__syscall_thread_create -Wl,--wrap=_open_r -Wl,--wrap=_write_r -Wl,--wrap=_close_r
    # src/switch/branch_trace.cpp: the indirect-site trace (M04-12); generated code calls this out of line
    -Wl,--wrap=_ZN10PS2Runtime23dispatchGuestBranchSlowEPhP12R5900ContextjjjNS_15GuestBranchKindEPKc
    ${NX_SDL2_STATIC_LDFLAGS_OTHER} ${NX_GL_STATIC_LDFLAGS_OTHER} ${NX_EGL_STATIC_LDFLAGS_OTHER})
# The generated archive is whole-archive: its registration constructor references every function. So is
# nx_game_private: vcs_overrides.cpp installs the whole game layer (applyVcs) through a static
# PS2_REGISTER_GAME_OVERRIDE object that nothing references, and a plain archive member like that is dropped by the
# linker - the game then boots without its hooks and waits forever in the PS2 movie player. Upstream's desktop build
# compiles these sources into the executable, so it never loses them. Portlibs are static; grouping resolves
# Mesa/SDL dependencies without depending on incidental archive ordering.
# The game layer, runtime, IOP and platform reference each other both ways: one group.
target_link_libraries(gtavcs PRIVATE glad
    -Wl,--whole-archive "${NX_PRIVATE_GENERATED_ARCHIVE}" $<TARGET_FILE:nx_game_private>
    $<TARGET_FILE:nx_self_registering> -Wl,--no-whole-archive
    -Wl,--start-group nx_game_private nx_self_registering nx_core nx_platform nx_imgui ps2_iop
    ${NX_SDL2_STATIC_LIBRARIES} ${NX_GL_STATIC_LIBRARIES} ${NX_EGL_STATIC_LIBRARIES} ${NX_ZLIB_STATIC_LIBRARIES}
    ${NX_FFMPEG_STATIC_LIBRARIES}
    "${NX_LIBRARY}" m -Wl,--end-group)

nx_generate_nacp(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gtavcs.nacp"
    NAME "GTA VCS NX" AUTHOR "gtavcs_nx contributors" VERSION "${PROJECT_VERSION}")
# NX_GAME_ICON: a local 256x256 JPEG for the homebrew menu (tools/build_game.py passes the ignored root icon.jpg when
# it exists); without it libnx's default icon.
set(NX_GAME_ICON "" CACHE FILEPATH "256x256 JPEG icon for gtavcs.nro (local, not committed)")
if(NX_GAME_ICON)
    set(nx_game_icon ICON "${NX_GAME_ICON}")
endif()
nx_create_nro(gtavcs OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gtavcs.nro"
    NACP "${CMAKE_CURRENT_BINARY_DIR}/gtavcs.nacp" ${nx_game_icon})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/gtavcs.nro" DESTINATION switch/gtavcs_nx)
