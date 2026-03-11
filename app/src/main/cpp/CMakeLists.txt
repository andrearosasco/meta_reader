cmake_minimum_required(VERSION 3.22.1)
project(meta_reader_native LANGUAGES C CXX)

add_library(
    android_native_app_glue
    STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

target_include_directories(
    android_native_app_glue
    PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue
)

find_package(OpenXR REQUIRED CONFIG)
find_library(android-lib android)
find_library(dl-lib dl)
find_library(log-lib log)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)

add_library(
    meta_reader
    SHARED
    main.cpp
    quest_passthrough_app.cpp
)

target_compile_features(meta_reader PRIVATE cxx_std_20)

target_compile_definitions(
    meta_reader
    PRIVATE
    XR_USE_PLATFORM_ANDROID
    XR_USE_GRAPHICS_API_OPENGL_ES
    XR_EXTENSION_PROTOTYPES
)

target_link_libraries(
    meta_reader
    PRIVATE
    android_native_app_glue
    OpenXR::headers
    OpenXR::openxr_loader
    ${android-lib}
    ${dl-lib}
    ${log-lib}
    ${egl-lib}
    ${gles-lib}
)
