# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Steering wheels go through SDL's joystick API (wheel_sdl.c) on every host
# that has SDL2. Windows links SDL2 statically, as the launcher does.
if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput1_4)
    find_package(SDL2 CONFIG QUIET)
    if(TARGET SDL2::SDL2-static)
        target_sources(xbox_input PRIVATE wheel_sdl.c)
        target_link_libraries(xbox_input PUBLIC SDL2::SDL2-static)
        target_compile_definitions(xbox_input PRIVATE XBOX_INPUT_WHEEL=1)
    else()
        message(STATUS "xbox_input: no static SDL2 -- no steering wheel support")
    endif()
else()
    find_package(SDL2 REQUIRED)
    target_sources(xbox_input PRIVATE wheel_sdl.c)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
    target_compile_definitions(xbox_input PRIVATE XBOX_INPUT_WHEEL=1)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
