# Cross build for 64-bit ARM Linux: clang and lld against a sysroot that
# holds the target's glibc, gcc runtime and libraries (SDL2, Vulkan loader
# and headers, glslang, libepoxy, OpenSSL). See RUNNING.md, "ARM64 test
# build", for making one and running the result under qemu-aarch64.
#
#   cmake -S rsc2 -B rsc2/build-arm64 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=rsc2/cmake/aarch64-linux.cmake \
#         -DRSC2_SYSROOT=/path/to/sysroot

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT RSC2_SYSROOT AND DEFINED ENV{RSC2_SYSROOT})
    set(RSC2_SYSROOT "$ENV{RSC2_SYSROOT}")
endif()
if(NOT RSC2_SYSROOT)
    message(FATAL_ERROR "aarch64-linux.cmake: set RSC2_SYSROOT to the target sysroot")
endif()
# try_compile projects get the toolchain file but not the cache.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RSC2_SYSROOT)

set(RSC2_TRIPLE aarch64-unknown-linux-gnu)
set(CMAKE_SYSROOT "${RSC2_SYSROOT}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${RSC2_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${RSC2_TRIPLE})
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")

# Libraries and headers from the sysroot only; programs (glslc) from the host.
set(CMAKE_FIND_ROOT_PATH "${RSC2_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${RSC2_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${RSC2_SYSROOT}/usr/lib/pkgconfig:${RSC2_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
