# ANGLE: OpenGL ES 3 on Direct3D 11, Vulkan or desktop GL, for the renderer.
# The Sneak King recomp's release build of ANGLE commit feebf60, downloaded at
# configure time and checked against its SHA-256. Defines the `angle` target.

set(ANGLE_URL "https://github.com/Guuzzeji/sneaky-whopper-recomp/releases/download/angle-feebf60/angle-feebf60-win-x64.zip")
set(ANGLE_SHA256 "ca8967062266827c5852db174919a46e31d690723b59313e0d71408aad84c5d7")
set(ANGLE_DIR "${CMAKE_BINARY_DIR}/_deps/angle")

if(NOT EXISTS "${ANGLE_DIR}/.extracted")
    set(_zip "${CMAKE_BINARY_DIR}/_deps/angle-feebf60-win-x64.zip")
    message(STATUS "Downloading ANGLE feebf60")
    file(DOWNLOAD "${ANGLE_URL}" "${_zip}" EXPECTED_HASH SHA256=${ANGLE_SHA256} STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        message(FATAL_ERROR "ANGLE download failed: ${_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${ANGLE_DIR}")
    file(TOUCH "${ANGLE_DIR}/.extracted")
endif()

file(GLOB_RECURSE _gles_lib "${ANGLE_DIR}/*libGLESv2.dll.lib")
list(GET _gles_lib 0 _gles_lib)
get_filename_component(_angle_root "${_gles_lib}" DIRECTORY)
get_filename_component(_angle_root "${_angle_root}" DIRECTORY)

add_library(angle INTERFACE)
target_include_directories(angle INTERFACE "${_angle_root}/include")
target_link_libraries(angle INTERFACE "${_angle_root}/lib/libEGL.dll.lib" "${_angle_root}/lib/libGLESv2.dll.lib")
set(ANGLE_RUNTIME_DLLS
    "${_angle_root}/bin/libEGL.dll"
    "${_angle_root}/bin/libGLESv2.dll"
    "${_angle_root}/bin/d3dcompiler_47.dll"
    "${_angle_root}/bin/vulkan-1.dll"
)
