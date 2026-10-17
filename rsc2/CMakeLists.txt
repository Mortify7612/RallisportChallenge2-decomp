cmake_minimum_required(VERSION 3.20)

# ============================================================================
# RalliSport Challenge 2 (Xbox, NTSC, title 0x4D530039) - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(rsc2 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Android: SDL2 from source (libSDL2.so, which SDLActivity loads), added
# before the toolkit so its find_package(SDL2) calls find these targets.
# See android/README.md.
if(ANDROID)
    set(RSC2_SDL2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/src/SDL2-2.32.10" CACHE PATH
        "SDL2 source tree for the Android build")
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RSC2_SDL2_SRC} ${CMAKE_BINARY_DIR}/SDL2)
    set(SDL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/android" CACHE PATH "" FORCE)
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/rsc2_overrides.c    # hand-written sub_XXXXXXXX (--exclude-manual)
    src/rsc2_profile.c      # RSC2_PROFILE sampling profiler (Linux)
    src/rsc2_callcount.c    # RSC2_CALLCOUNT (needs tools/callcount_instrument.py on)
    src/launcher.c          # pre-game settings window + settings.ini
    src/steam_shortcut.c    # launcher's "Add to Steam" (shortcuts.vdf)
    src/host_coro.c         # fiber stacks where there is no ucontext (Android)
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

if(ANDROID)
    # SDLActivity loads libmain.so and calls SDL_main (main.c).
    add_library(${PROJECT_NAME} SHARED ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME main)
    target_link_libraries(${PROJECT_NAME} PRIVATE android log)
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,--no-undefined)
else()
add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)
endif()

# host_coro.c in place of ucontext for the fiber HLE: always on Android,
# where bionic has no ucontext; on Linux only to test it.
option(RSC2_HOST_CORO "Fibers on host_coro.c instead of ucontext" OFF)
if(RSC2_HOST_CORO)
    set_property(SOURCE src/rsc2_overrides.c APPEND PROPERTY COMPILE_DEFINITIONS RSC2_HOST_CORO=1)
endif()
if(WIN32)
    set_source_files_properties(src/host_coro.c PROPERTIES HEADER_FILE_ONLY ON)
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

if(WIN32)
    # Windows SDK libraries needed at link time
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        xinput1_4   # in Windows 8+; xinput1_3 needs the old DirectX runtime
        winmm
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
    )
else()
    # The OpenGL/SDL2 backend comes in through the xboxrecomp targets.
    # -rdynamic puts every sub_XXXXXXXX in the dynamic symbol table, which is
    # what lets the fault handler name the guest function with dladdr().
    target_link_libraries(${PROJECT_NAME} PRIVATE m dl)
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
    # Generated code is a register-model translation: aliasing through MEM32()
    # and signed wraparound are the point, not bugs.
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-fno-strict-aliasing;-fwrapv;-w")
endif()

# MinGW (the Linux-hosted Windows cross build): the same code-generation
# contract as GCC elsewhere, and a self-contained .exe -- without -static it
# needs libwinpthread-1.dll and libgcc's DLL next to it.
if(MINGW)
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-fno-strict-aliasing;-fwrapv;-w")
    target_link_options(${PROJECT_NAME} PRIVATE -static -static-libgcc)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# The launcher embeds its fonts (generated headers, so any C compiler builds
# it) and uses stb_truetype and SDL2. On POSIX hosts SDL2 is there already
# (the runtime uses it); a Windows build gets the launcher window when it can
# find a static SDL2 (-DSDL2_DIR=.../lib/cmake/SDL2, see RUNNING.md) and only
# applies the saved settings without one.
set(RSC2_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/rsc2_gen)
file(MAKE_DIRECTORY ${RSC2_GEN_DIR})
foreach(font Regular Bold)
    add_custom_command(
        OUTPUT ${RSC2_GEN_DIR}/font_${font}.h
        COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSans-${font}.ttf
                -DOUT=${RSC2_GEN_DIR}/font_${font}.h -DNAME=k_font_${font}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_bin.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSans-${font}.ttf
                ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_bin.cmake
        COMMENT "embed NotoSans-${font}.ttf")
endforeach()
target_sources(${PROJECT_NAME} PRIVATE ${RSC2_GEN_DIR}/font_Regular.h ${RSC2_GEN_DIR}/font_Bold.h)
set_source_files_properties(src/launcher.c PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb;${RSC2_GEN_DIR}")
if(NOT WIN32)
    find_package(SDL2 REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RSC2_LAUNCHER_SDL=1)
else()
    find_package(SDL2 CONFIG QUIET)
    if(TARGET SDL2::SDL2-static)
        message(STATUS "rsc2: launcher window with static SDL2 ${SDL2_VERSION}")
        target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2-static)
        target_compile_definitions(${PROJECT_NAME} PRIVATE RSC2_LAUNCHER_SDL=1)
    else()
        message(STATUS "rsc2: no static SDL2 -- the launcher window is left out")
    endif()
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
