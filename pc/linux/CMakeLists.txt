# RinCast Streamer for Linux (docs/LINUX.md). The session layer is the Windows streamer's own
# code; what sits under it comes from pc/streamer/src/platform/linux.
#
#   cmake -S pc/linux -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/linux
cmake_minimum_required(VERSION 3.22)
project(rincast-linux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(RC_TP ${RC_ROOT}/third_party)
set(RC_SRC ${RC_ROOT}/pc/streamer/src)
set(RC_LINUX ${RC_SRC}/platform/linux)
set(RC_POSIX ${RC_SRC}/platform/posix)

file(READ "${RC_ROOT}/VERSION" RINCAST_VERSION)
string(STRIP "${RINCAST_VERSION}" RINCAST_VERSION)

add_subdirectory(${RC_ROOT}/shared ${CMAKE_BINARY_DIR}/rincast-core)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

add_executable(rincast-streamer
  # Shared with Windows
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
  # Linux
  ${RC_LINUX}/util_linux.cpp
  ${RC_LINUX}/gpu_linux.cpp
  ${RC_LINUX}/monitors_linux.cpp
  ${RC_LINUX}/desktop_stream_linux.cpp
  ${RC_LINUX}/input_linux.cpp
  ${RC_LINUX}/audio_linux.cpp
  ${RC_LINUX}/encoder_linux.cpp
)
target_include_directories(rincast-streamer PRIVATE ${RC_SRC} ${RC_ROOT}/pc/common ${RC_TP})
target_link_libraries(rincast-streamer PRIVATE rincast-core Threads::Threads)
target_compile_options(rincast-streamer PRIVATE -Wall -Wno-unused-parameter)

# Optional until the packages in docs/LINUX.md are installed: without them the streamer still
# builds, and says in its log what it can't do.
pkg_check_modules(CURL IMPORTED_TARGET libcurl)
if(CURL_FOUND)
  target_compile_definitions(rincast-streamer PRIVATE RINCAST_HAVE_CURL=1)
  target_link_libraries(rincast-streamer PRIVATE PkgConfig::CURL)
endif()

# The web UI next to the executable, as on Windows.
add_custom_command(TARGET rincast-streamer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${RC_ROOT}/pc/streamer/ui $<TARGET_FILE_DIR:rincast-streamer>/ui
  COMMAND ${CMAKE_COMMAND} -E copy ${RC_ROOT}/assets/brand/mark-256.png $<TARGET_FILE_DIR:rincast-streamer>/ui/mark.png
)
