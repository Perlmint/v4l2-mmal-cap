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
