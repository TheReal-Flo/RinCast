# RinCast PC components: streamer, SteamVR driver, OpenXR runtime backend.
cmake_minimum_required(VERSION 3.22)
project(RinCastPC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(RINCAST_BUILD_OPENXR "Build RinCast XR and xr-smoke (local builds only, not in public releases)" OFF)

get_filename_component(RC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# VERSION is the single source of the version; streamer/src/version.h is stamped from it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RC_ROOT}/VERSION")
file(READ "${RC_ROOT}/VERSION" RINCAST_VERSION)
string(STRIP "${RINCAST_VERSION}" RINCAST_VERSION)
if(NOT RINCAST_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  message(FATAL_ERROR "VERSION must be major.minor.patch, got '${RINCAST_VERSION}'")
endif()
set(_version_h "#pragma once

#define RINCAST_VERSION_MAJOR ${CMAKE_MATCH_1}
#define RINCAST_VERSION_MINOR ${CMAKE_MATCH_2}
#define RINCAST_VERSION_PATCH ${CMAKE_MATCH_3}
#define RINCAST_VERSION_STRING \"${RINCAST_VERSION}\"
")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/streamer/src/version.h" _version_h_current)
if(NOT _version_h_current STREQUAL _version_h)
  file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/streamer/src/version.h" "${_version_h}")
endif()

# Keep the build machine's paths out of the binaries (__FILE__, asserts). The more
# specific build folder comes first because it can sit inside the source tree.
if(MSVC)
  add_compile_options(/experimental:deterministic)  # /pathmap is ignored without it
  foreach(_map "${CMAKE_BINARY_DIR}=rincast-build" "${RC_ROOT}=rincast")
    file(TO_NATIVE_PATH "${_map}" _map_native)
    add_compile_options("/pathmap:${_map_native}" "/pathmap:${_map}")
  endforeach()
endif()

add_subdirectory(../shared ${CMAKE_BINARY_DIR}/shared)
add_subdirectory(streamer)
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/driver-openvr/CMakeLists.txt)
  add_subdirectory(driver-openvr)
endif()
if(RINCAST_BUILD_OPENXR AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/runtime-openxr/CMakeLists.txt)
  add_subdirectory(runtime-openxr)
elseif(RINCAST_BUILD_OPENXR)
  message(WARNING "RINCAST_BUILD_OPENXR is ON but pc/runtime-openxr is missing; skipping RinCast XR.")
endif()

# RinCast's setup and uninstaller; scripts/build-all.ps1 packs the release files into it.
add_executable(rincast-setup WIN32 installer/setup/setup.cpp installer/setup/setup.rc)
set_target_properties(rincast-setup PROPERTIES OUTPUT_NAME "RinCast Setup")
target_compile_definitions(rincast-setup PRIVATE UNICODE _UNICODE)
target_link_libraries(rincast-setup PRIVATE dwmapi shell32 ole32)
target_link_options(rincast-setup PRIVATE /MANIFEST:NO)

# Developer tool: simulates a headset against a running streamer.
add_executable(headset-sim ../tools/headset-sim/main.cpp)
target_link_libraries(headset-sim PRIVATE rincast-core)
add_executable(pose-probe ../tools/pose-probe/main.cpp)
target_include_directories(pose-probe PRIVATE ../third_party)
target_link_libraries(pose-probe PRIVATE d3d11)

# Developer tool: checks the desktop capture -> YUV pipeline.
add_executable(capture-test ../tools/capture-test/main.cpp
  streamer/src/capture/desktop_capture.cpp streamer/src/capture/monitors.cpp
  streamer/src/encode/color_convert.cpp streamer/src/gpu.cpp streamer/src/util.cpp)
target_include_directories(capture-test PRIVATE ../third_party)
target_compile_definitions(capture-test PRIVATE UNICODE _UNICODE)
target_link_libraries(capture-test PRIVATE rincast-core)

# Developer tool: frame generation quality and estimator speed on this GPU.
add_executable(motion-bench ../tools/motion-bench/main.cpp streamer/src/stream/motion_estimator.cpp)
target_include_directories(motion-bench PRIVATE ../third_party)
target_compile_definitions(motion-bench PRIVATE UNICODE _UNICODE)
target_link_libraries(motion-bench PRIVATE d3d11 dxgi d3dcompiler)

# Developer tool: encoder speed / frame size / loss recovery on this GPU.
add_executable(encode-bench ../tools/encode-bench/main.cpp
  streamer/src/capture/desktop_capture.cpp streamer/src/capture/monitors.cpp
  streamer/src/encode/color_convert.cpp streamer/src/encode/encoder.cpp streamer/src/encode/nvenc_encoder.cpp
  streamer/src/encode/amf_encoder.cpp streamer/src/encode/mf_encoder.cpp streamer/src/gpu.cpp streamer/src/util.cpp)
target_include_directories(encode-bench PRIVATE ../third_party)
target_compile_definitions(encode-bench PRIVATE UNICODE _UNICODE)
target_link_libraries(encode-bench PRIVATE rincast-core)

# Developer tool: lists the virtual audio devices and how each setting resolves.
add_executable(audio-check ../tools/audio-check/main.cpp streamer/src/audio/audio.cpp streamer/src/util.cpp)
target_include_directories(audio-check PRIVATE streamer/src ../third_party)
target_compile_definitions(audio-check PRIVATE UNICODE _UNICODE)
target_link_libraries(audio-check PRIVATE rincast-core avrt ole32)

# Developer tool: minimal OpenXR app that drives the RinCast runtime directly.
if(RINCAST_BUILD_OPENXR AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/../tools/xr-smoke/main.cpp)
  add_executable(xr-smoke ../tools/xr-smoke/main.cpp)
  target_include_directories(xr-smoke PRIVATE ../third_party/openxr)
endif()

# Installs/removes the RinCast Virtual Audio device (the driver itself is
# built separately from pc/audio-driver with the WDK).
add_executable(rincast-audio-setup audio-driver/setup/audio_setup.cpp)
target_compile_definitions(rincast-audio-setup PRIVATE UNICODE _UNICODE)

# Host tool: bakes SDF text/icon atlases for the headset UI.
add_executable(assetbaker ../tools/assetbaker/main.cpp)
target_include_directories(assetbaker PRIVATE ../third_party)

# Developer tool: read-only grab of the newest VR bridge frame + metadata.
add_executable(bridge-grab ../tools/bridge-grab/main.cpp)
target_include_directories(bridge-grab PRIVATE common ../third_party)
target_compile_definitions(bridge-grab PRIVATE UNICODE _UNICODE)

# Developer tool: tests router port mapping (PCP / NAT-PMP / UPnP) and removes it again.
add_executable(portmap-test ../tools/portmap-test/main.cpp streamer/src/net/port_mapper.cpp
  streamer/src/net/http.cpp streamer/src/util.cpp)
target_include_directories(portmap-test PRIVATE streamer/src ../third_party)
target_compile_definitions(portmap-test PRIVATE UNICODE _UNICODE)
target_link_libraries(portmap-test PRIVATE rincast-core winhttp iphlpapi)

# Developer tool: end-to-end remote connection test (local server + streamer + the
# headset app's real StreamClient). See docs/BUILDING.md.
add_executable(remote-e2e ../tools/remote-e2e/main.cpp ../headset/app/src/main/cpp/net/stream_client.cpp
  streamer/src/net/http.cpp streamer/src/util.cpp)
target_include_directories(remote-e2e PRIVATE streamer/src ../headset/app/src/main/cpp ../third_party)
target_compile_definitions(remote-e2e PRIVATE UNICODE _UNICODE)
target_link_libraries(remote-e2e PRIVATE rincast-core winhttp crypt32)
