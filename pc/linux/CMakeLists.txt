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

# Every file in platform/linux is Linux's part.
file(GLOB RC_LINUX_SOURCES CONFIGURE_DEPENDS ${RC_LINUX}/*.cpp)

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
  ${RC_SRC}/vr/monado.cpp
  ${RC_SRC}/cloud/cloud_client.cpp
  ${RC_SRC}/games/game_library.cpp
  # Linux and macOS
  ${RC_POSIX}/main_terminal.cpp
  ${RC_POSIX}/util_posix.cpp
  ${RC_POSIX}/http_curl.cpp
  ${RC_POSIX}/system_posix.cpp
  # Linux
  ${RC_LINUX_SOURCES}
)
target_include_directories(rincast-streamer PRIVATE ${RC_SRC} ${RC_ROOT}/pc/common ${RC_TP})
target_link_libraries(rincast-streamer PRIVATE rincast-core Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(rincast-streamer PRIVATE -Wall -Wno-unused-parameter)

# The packages in docs/LINUX.md.
pkg_check_modules(RC_LINUX_DEPS REQUIRED IMPORTED_TARGET
  libcurl
  libavcodec libavutil libavfilter libswscale libva libva-drm libdrm
  x11 xext xfixes xtst xrandr xdamage
  gio-2.0 gio-unix-2.0 libpipewire-0.3 libei-1.0 wayland-client libpulse
  gtk+-3.0 webkit2gtk-4.1 ayatana-appindicator3-0.1
  vulkan
)
# RINCAST_GUI_MAIN: main() is the GTK window's (main_gtk.cpp), which falls back to the terminal
# front end (main_terminal.cpp's RunInTerminal) without a display.
target_compile_definitions(rincast-streamer PRIVATE RINCAST_HAVE_CURL=1 RINCAST_GUI_MAIN=1)
target_link_libraries(rincast-streamer PRIVATE PkgConfig::RC_LINUX_DEPS)

# The web UI next to the executable, as on Windows.
add_custom_command(TARGET rincast-streamer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${RC_ROOT}/pc/streamer/ui $<TARGET_FILE_DIR:rincast-streamer>/ui
  COMMAND ${CMAKE_COMMAND} -E copy ${RC_ROOT}/assets/brand/mark-256.png $<TARGET_FILE_DIR:rincast-streamer>/ui/mark.png
)
