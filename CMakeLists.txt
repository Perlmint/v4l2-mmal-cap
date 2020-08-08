cmake_minimum_required(VERSION 3.13)
project(v4l2-mmal-cap VERSION 0.1.0 LANGUAGES CXX)

include(FindPkgConfig)

# Default install path opn raspi-os
list(APPEND CMAKE_PREFIX_PATH /opt/vc)

pkg_search_module(BCM_HOST REQUIRED IMPORTED_TARGET bcm_host)
pkg_search_module(MMAL REQUIRED IMPORTED_TARGET mmal)

add_executable(v4l2-mmal-cap
    main.cpp
    camera.cpp camera.h
    encoder.cpp encoder.h)
target_compile_features(v4l2-mmal-cap
    PRIVATE cxx_std_17)
target_link_libraries(v4l2-mmal-cap
    PkgConfig::MMAL PkgConfig::BCM_HOST pthread stdc++fs)

# set variables for addon archive
set(ADDON_NAME kr.perlmint.rpi.capture)
set(BINARY_NAME v4l2-mmal-cap)
set(BINARY_DIR ${CMAKE_BINARY_DIR})
set(BINARY_PATH ${BINARY_DIR}/${BINARY_NAME})
find_program(ZIP_BIN zip)
if(ZIP_BIN)
    set(ARCHIVER_TYPE zip)
endif()
configure_file(
    ${CMAKE_SOURCE_DIR}/pack-kodi-addon.cmake.in
    ${CMAKE_BINARY_DIR}/pack-kodi-addon.cmake
    @ONLY
)

add_custom_target(kodi-addon
    cmake -P ${CMAKE_BINARY_DIR}/pack-kodi-addon.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/${ADDON_NAME}.zip)
add_dependencies(kodi-addon v4l2-mmal-cap)