cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces 147 Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_path.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_thread.c
    kernel_sync.c
    kernel_hal.c
    kernel_xbox.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    nv2a_irq.c
    xbox_memory_layout.c
    guest_vmem.c
    icall_feedback.c
    recomp_trace.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c
    nv2a_vsh_interp.c
    nv2a_combiner.c
    nv2a_vk.c
)

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})

# Vulkan renderer for the pushbuffer executor (nv2a_vk.c, RECOMP_VULKAN=1).
# Optional: without the Vulkan headers/loader or glslc, nv2a_vk.c builds as
# stubs and the software rasteriser is the only renderer.
find_package(Vulkan QUIET)
find_program(XBOXRECOMP_GLSLC glslc)
if(Vulkan_FOUND AND XBOXRECOMP_GLSLC)
    set(NV2A_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
    set(NV2A_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/nv2a_shaders)
    file(MAKE_DIRECTORY ${NV2A_SHADER_OUT})
    foreach(stage vert frag)
        add_custom_command(
            OUTPUT ${NV2A_SHADER_OUT}/nv2a_${stage}.spv.h
            COMMAND ${XBOXRECOMP_GLSLC} --target-env=vulkan1.3 -O -mfmt=c
                    -I ${NV2A_SHADER_DIR} ${NV2A_SHADER_DIR}/nv2a.${stage}
                    -o ${NV2A_SHADER_OUT}/nv2a_${stage}.spv.h
            DEPENDS ${NV2A_SHADER_DIR}/nv2a.${stage}
                    ${NV2A_SHADER_DIR}/nv2a_common.glsl
            COMMENT "glslc nv2a.${stage}")
    endforeach()
    # The fragment shader again without its gl_FragDepth write (nv2a_vk.c
    # draws with it whenever the rasteriser's depth is the right one).
    add_custom_command(
        OUTPUT ${NV2A_SHADER_OUT}/nv2a_frag_fast.spv.h
        COMMAND ${XBOXRECOMP_GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DNV2A_FAST_DEPTH
                -fshader-stage=frag -I ${NV2A_SHADER_DIR} ${NV2A_SHADER_DIR}/nv2a.frag
                -o ${NV2A_SHADER_OUT}/nv2a_frag_fast.spv.h
        DEPENDS ${NV2A_SHADER_DIR}/nv2a.frag ${NV2A_SHADER_DIR}/nv2a_common.glsl
        COMMENT "glslc nv2a.frag (fast depth)")
    # The fragment shader's source too: nv2a_vk.c compiles it again with the
    # combiner setup of a draw as constants (specialised shaders).
    foreach(src nv2a.frag nv2a_common.glsl)
        string(REPLACE "." "_" sym "${src}")
        add_custom_command(
            OUTPUT ${NV2A_SHADER_OUT}/${sym}_src.h
            COMMAND ${CMAKE_COMMAND} -DIN=${NV2A_SHADER_DIR}/${src}
                    -DOUT=${NV2A_SHADER_OUT}/${sym}_src.h -DNAME=k_${sym}_src
                    -P ${NV2A_SHADER_DIR}/embed.cmake
            DEPENDS ${NV2A_SHADER_DIR}/${src} ${NV2A_SHADER_DIR}/embed.cmake
            COMMENT "embed ${src}")
    endforeach()
    target_sources(xbox_kernel PRIVATE
        ${NV2A_SHADER_OUT}/nv2a_vert.spv.h ${NV2A_SHADER_OUT}/nv2a_frag.spv.h
        ${NV2A_SHADER_OUT}/nv2a_frag_fast.spv.h
        ${NV2A_SHADER_OUT}/nv2a_frag_src.h ${NV2A_SHADER_OUT}/nv2a_common_glsl_src.h)
    target_include_directories(xbox_kernel PRIVATE ${NV2A_SHADER_OUT})
    target_compile_definitions(xbox_kernel PRIVATE XBOXRECOMP_HAVE_VULKAN=1)
    target_link_libraries(xbox_kernel PUBLIC Vulkan::Vulkan)
    # `VkFooInfo x = { VK_STRUCTURE_TYPE_FOO }` is the idiom; the rest is zero.
    set_source_files_properties(nv2a_vk.c PROPERTIES
        COMPILE_OPTIONS "-Wno-missing-field-initializers")
    # Title vertex programs run on the GPU once translated to GLSL and
    # compiled at runtime with glslang's C interface; without it only
    # programs already in the shader cache do (the rest use the CPU).
    find_path(XBOXRECOMP_GLSLANG_INCLUDE glslang/Include/glslang_c_interface.h)
    find_library(XBOXRECOMP_GLSLANG_LIB NAMES glslang)
    find_library(XBOXRECOMP_GLSLANG_RL_LIB NAMES glslang-default-resource-limits)
    if(XBOXRECOMP_GLSLANG_INCLUDE AND XBOXRECOMP_GLSLANG_LIB AND XBOXRECOMP_GLSLANG_RL_LIB)
        target_include_directories(xbox_kernel PRIVATE ${XBOXRECOMP_GLSLANG_INCLUDE})
        target_compile_definitions(xbox_kernel PRIVATE XBOXRECOMP_HAVE_GLSLANG=1)
        target_link_libraries(xbox_kernel PUBLIC ${XBOXRECOMP_GLSLANG_LIB}
                              ${XBOXRECOMP_GLSLANG_RL_LIB} ${XBOXRECOMP_GLSLANG_EXTRA_LIBS})
        message(STATUS "xboxrecomp: glslang ${XBOXRECOMP_GLSLANG_LIB}")
    endif()
    message(STATUS "xboxrecomp: Vulkan renderer enabled (RECOMP_VULKAN=1)")
else()
    message(STATUS "xboxrecomp: Vulkan renderer disabled (no Vulkan SDK/glslc)")
endif()

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)

if(WIN32)
    # platform is an INTERFACE target on Windows: it contributes the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt)
elseif(ANDROID)
    # The Xbox crypto (kernel_crypto.c) is software; nothing needs OpenSSL.
    target_link_libraries(xbox_kernel PUBLIC platform)
else()
    # Linux: platform layer (POSIX host primitives) + OpenSSL for crypto.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)
endif()

if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- /O2 /GS- /Gy)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
