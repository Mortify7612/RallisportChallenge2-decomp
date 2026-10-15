cmake_minimum_required(VERSION 3.20)
project(platform_veh_posix_test C)
# Vectored exception handlers and handle validation in the POSIX Win32 shim.
# Needs no guest window or game files: it is the shim alone.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX shim")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../../src/platform platform)
add_executable(platform_veh_posix_test test_main.c)
target_link_libraries(platform_veh_posix_test PRIVATE platform)
enable_testing()
add_test(NAME platform_veh_posix_test COMMAND platform_veh_posix_test)
