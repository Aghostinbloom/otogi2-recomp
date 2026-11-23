cmake_minimum_required(VERSION 3.25)
project(otogi2_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

add_subdirectory(xboxrecomp)
include(cmake/angle.cmake)

file(GLOB OTOGI2_GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen/*.c")
if(NOT OTOGI2_GEN_SOURCES)
    message(FATAL_ERROR "src/recomp/gen/ is empty: lift the game first (docs/NOTES.md, Lift).")
endif()

add_executable(otogi2
    src/host/main.cpp
    src/host/crash.cpp
    src/host/manual.cpp
    src/host/stacks.cpp
    src/host/window.cpp
    src/render/gl_hle.c
    src/games/otogi2/overrides.cpp
    src/games/otogi2/d3d.cpp
    ${OTOGI2_GEN_SOURCES}
)

# Lifted code relies on wrapping arithmetic and on aliasing through casts.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(${OTOGI2_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;-fwrapv;-fno-strict-aliasing")
else()
    set_source_files_properties(${OTOGI2_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
endif()

target_include_directories(otogi2 PRIVATE
    src
    src/recomp/gen
    src/render
    src/games/otogi2
    xboxrecomp/include
    xboxrecomp/src/kernel
    xboxrecomp/src/d3d
)
target_compile_definitions(otogi2 PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(otogi2 PRIVATE /bigobj)
target_link_options(otogi2 PRIVATE /LARGEADDRESSAWARE)
target_link_libraries(otogi2 PRIVATE
    xboxrecomp
    angle
    d3d11 dxgi dxguid xinput winmm dbghelp shell32 ole32
)
add_custom_command(TARGET otogi2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ANGLE_RUNTIME_DLLS} $<TARGET_FILE_DIR:otogi2>
)
