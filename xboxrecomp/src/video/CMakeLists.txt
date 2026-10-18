# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
else()
    # The framebuffer window (fb_present.c) is SDL2 on POSIX hosts.
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(xbox_video PUBLIC SDL2::SDL2 Threads::Threads)
endif()
# The OpenGL renderer (kernel/nv2a_gl.c) presents in a GL window made here.
if(XBOXRECOMP_HAVE_GL)
    target_compile_definitions(xbox_video PRIVATE XBOXRECOMP_HAVE_GL=1)
endif()
