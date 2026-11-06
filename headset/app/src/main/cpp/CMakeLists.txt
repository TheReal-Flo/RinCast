# RinCast headset app (native part of the Android package).
cmake_minimum_required(VERSION 3.22)
project(rincast-headset CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../..)
add_subdirectory(${RC_ROOT}/shared ${CMAKE_CURRENT_BINARY_DIR}/rincast-core)

find_package(OpenXR REQUIRED CONFIG)

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(rincast SHARED
  app/app.cpp
  app/screens.cpp
  app/settings.cpp
  gfx/gl.cpp
  gfx/scene.cpp
  gfx/sky.cpp
  gfx/ui_renderer.cpp
  media/audio.cpp
  media/music.cpp
  media/video_decoder.cpp
  net/cloud.cpp
  net/stream_client.cpp
  platform/assets.cpp
  platform/jni_bridge.cpp
  ui/ui.cpp
  ui/window.cpp
  xr/xr_context.cpp
  xr/xr_input.cpp
  # What differs between headsets; see headsets/headset.h.
  headsets/headset.cpp
  headsets/quest/quest_headset.cpp
  headsets/quest/quest_body.cpp
  headsets/quest/quest_face.cpp
  headsets/pico/pico_headset.cpp
  headsets/pico/pico_body.cpp
  headsets/pico/pico_face.cpp
  stb_impl.cpp
)
target_compile_options(rincast PRIVATE -Wall -Wno-unused-parameter -Wno-missing-field-initializers -fno-strict-aliasing)
target_include_directories(rincast PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${RC_ROOT}/third_party)
# The glue's entry point must not be stripped.
set_target_properties(rincast PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
target_link_libraries(rincast PRIVATE
  rincast-core
  native_app_glue
  OpenXR::openxr_loader
  android log EGL GLESv3 mediandk aaudio nativewindow jnigraphics
)
