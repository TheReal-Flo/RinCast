# RinCast Streamer for macOS (pc/mac/README.md). The session layer is the Windows streamer's own
# code, as on Linux; what sits under it comes from pc/streamer/src/platform/mac (ScreenCaptureKit,
# VideoToolbox, CGEvent input) and platform/posix (what Linux and macOS share).
#
#   cmake -S pc/mac -B build-mac -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-mac
cmake_minimum_required(VERSION 3.22)
project(rincast-mac LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

set(RC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(RC_TP ${RC_ROOT}/third_party)
set(RC_SRC ${RC_ROOT}/pc/streamer/src)
set(RC_MAC ${RC_SRC}/platform/mac)
set(RC_POSIX ${RC_SRC}/platform/posix)

if(NOT TARGET rincast-core)
  add_subdirectory(${RC_ROOT}/shared ${CMAKE_BINARY_DIR}/rincast-core)
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)  # part of the macOS SDK

add_executable(rincast-streamer
  # Shared with Windows and Linux
  ${RC_SRC}/app.cpp
  ${RC_SRC}/settings.cpp
  ${RC_SRC}/util_common.cpp
  ${RC_SRC}/session/client_session.cpp
  ${RC_SRC}/net/stream_server.cpp
  ${RC_SRC}/net/port_mapper.cpp
  ${RC_SRC}/stream/video_sender.cpp
  ${RC_SRC}/stream/desktop_stream_common.cpp
  ${RC_SRC}/vr/vr_bridge_host.cpp
  ${RC_SRC}/cloud/cloud_client.cpp
  ${RC_SRC}/games/game_library.cpp
  # Linux and macOS
  ${RC_POSIX}/main_terminal.cpp
  ${RC_POSIX}/util_posix.cpp
  ${RC_POSIX}/http_curl.cpp
  ${RC_POSIX}/system_posix.cpp
  ${RC_POSIX}/vr_stream_unavailable.cpp
  ${RC_POSIX}/audio_devices_none.cpp
  # macOS
  ${RC_MAC}/util_mac.mm
  ${RC_MAC}/gpu_mac.mm
  ${RC_MAC}/monitors_mac.mm
  ${RC_MAC}/desktop_stream_mac.mm
  ${RC_MAC}/vt_encoder.mm
  ${RC_MAC}/input_mac.mm
  ${RC_MAC}/audio_mac.mm
)
target_include_directories(rincast-streamer PRIVATE ${RC_SRC} ${RC_ROOT}/pc/common ${RC_TP})
target_compile_definitions(rincast-streamer PRIVATE RINCAST_HAVE_CURL=1)
target_compile_options(rincast-streamer PRIVATE
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  -Wall -Wno-unused-parameter
)
target_link_libraries(rincast-streamer PRIVATE
  rincast-core
  Threads::Threads
  CURL::libcurl
  "-framework AppKit"
  "-framework ApplicationServices"
  "-framework Carbon"
  "-framework CoreGraphics"
  "-framework CoreMedia"
  "-framework CoreVideo"
  "-framework IOKit"
  "-framework Metal"
  "-framework ScreenCaptureKit"
  "-framework VideoToolbox"
)
