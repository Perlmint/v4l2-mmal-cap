cmake_minimum_required(VERSION 3.13)
project(v4l2-mmal-cap VERSION 0.1.0 LANGUAGES CXX)

add_executable(v4l2-mmal-cap
    main.cpp
    camera.cpp camera.h
    encoder.cpp encoder.h)
target_compile_features(v4l2-mmal-cap
    PRIVATE cxx_std_17)
target_link_libraries(v4l2-mmal-cap
    mmal bcm_host vcos vchiq_arm mmal mmal_core mmal_util mmal_vc_client pthread stdc++fs)
target_link_directories(v4l2-mmal-cap
    PRIVATE /opt/vc/lib
)
target_include_directories(v4l2-mmal-cap SYSTEM
    PRIVATE /opt/vc/include
)