cmake_minimum_required(VERSION 3.22.1)
project(pixelvr_openxr LANGUAGES CXX)

find_package(OpenXR REQUIRED)

add_library(pixelvr_openxr SHARED openxr_session.cpp)
target_compile_features(pixelvr_openxr PRIVATE cxx_std_17)
target_compile_definitions(pixelvr_openxr PRIVATE
    XR_USE_PLATFORM_ANDROID
    XR_USE_GRAPHICS_API_OPENGL_ES
)
target_link_libraries(pixelvr_openxr PRIVATE
    OpenXR::openxr_loader
    android
    log
    EGL
    GLESv3
)
