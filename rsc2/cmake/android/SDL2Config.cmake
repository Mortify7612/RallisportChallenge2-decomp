# The Android build compiles SDL2 from source (rsc2/CMakeLists.txt adds it
# before the toolkit), so its targets already exist; this lets the toolkit's
# find_package(SDL2) calls succeed without an installed SDL2.
if(NOT TARGET SDL2::SDL2)
    message(FATAL_ERROR "SDL2Config (Android): SDL2 must be added with add_subdirectory first")
endif()
set(SDL2_FOUND TRUE)
