# RinCast for the Steam Frame: the headset app as a native Linux ARM64 program (SteamOS), built in
# Steam's Sniper SDK with build.sh. The same code as the Android app; what differs is in the
# Linux files listed below (platform/linux, media/video_decoder_v4l2.cpp) and behind
# `#ifdef __ANDROID__` in the rest.
cmake_minimum_required(VERSION 3.22)
project(rincast-frame CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(RC_APP ${RC_ROOT}/headset/app/src/main/cpp)
add_subdirectory(${RC_ROOT}/shared ${CMAKE_CURRENT_BINARY_DIR}/rincast-core)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RC_ROOT}/VERSION")
file(READ "${RC_ROOT}/VERSION" RINCAST_VERSION)
string(STRIP "${RINCAST_VERSION}" RINCAST_VERSION)

add_executable(rincast-frame
  ${RC_APP}/app/app.cpp
  ${RC_APP}/app/screens.cpp
  ${RC_APP}/app/settings.cpp
  ${RC_APP}/gfx/gl.cpp
  ${RC_APP}/gfx/scene.cpp
  ${RC_APP}/gfx/sky.cpp
  ${RC_APP}/gfx/ui_renderer.cpp
  ${RC_APP}/media/audio.cpp
  ${RC_APP}/media/music.cpp
  ${RC_APP}/media/video_decoder_common.cpp
  ${RC_APP}/media/video_decoder_v4l2.cpp
  ${RC_APP}/net/cloud.cpp
  ${RC_APP}/net/stream_client.cpp
  ${RC_APP}/platform/linux/platform_linux.cpp
  ${RC_APP}/platform/linux/assets_linux.cpp
  ${RC_APP}/ui/ui.cpp
  ${RC_APP}/ui/window.cpp
  ${RC_APP}/xr/xr_context.cpp
  ${RC_APP}/xr/xr_input.cpp
  ${RC_APP}/xr/xr_loader.cpp
  ${RC_APP}/headsets/headset.cpp
  ${RC_APP}/headsets/quest/quest_headset.cpp
  ${RC_APP}/headsets/quest/quest_body.cpp
  ${RC_APP}/headsets/quest/quest_face.cpp
  ${RC_APP}/headsets/pico/pico_headset.cpp
  ${RC_APP}/headsets/pico/pico_body.cpp
  ${RC_APP}/headsets/pico/pico_face.cpp
  ${RC_APP}/headsets/androidxr/androidxr_headset.cpp
  ${RC_APP}/stb_impl.cpp
)
target_include_directories(rincast-frame PRIVATE ${RC_APP} ${RC_ROOT}/third_party ${RC_ROOT}/third_party/openxr)
# EGL_NO_X11: the EGL headers would otherwise pull in Xlib, whose Window type and None macro clash
# with RinCast's own.
target_compile_definitions(rincast-frame PRIVATE RINCAST_VERSION_STRING="${RINCAST_VERSION}" EGL_NO_X11 MESA_EGL_NO_X11_HEADERS)
target_compile_options(rincast-frame PRIVATE -Wall -Wno-unused-parameter -Wno-missing-field-initializers -fno-strict-aliasing)
# The OpenXR loader is opened at run time (SteamVR's, see xr/xr_loader.cpp); EGL, GL ES, PulseAudio
# and curl come from the Steam Runtime.
target_link_libraries(rincast-frame PRIVATE rincast-core EGL GLESv2 pulse-simple pulse curl-gnutls dl pthread)

# The app's assets (skies, UI atlas, music) next to the program, where platform/linux looks.
add_custom_command(TARGET rincast-frame POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${RC_ROOT}/headset/app/src/main/assets $<TARGET_FILE_DIR:rincast-frame>/assets)
