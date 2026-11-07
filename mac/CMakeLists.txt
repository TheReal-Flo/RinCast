cmake_minimum_required(VERSION 3.22)
project(rincast-streamer-mac LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RC_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(RC_SHARED ${RC_ROOT}/shared)
set(RC_THIRD_PARTY ${RC_ROOT}/third_party)

# Add shared core if not already added
if(NOT TARGET rincast-core)
    add_subdirectory(${RC_SHARED} ${CMAKE_CURRENT_BINARY_DIR}/shared)
endif()

set(MAC_STREAMER_SOURCES
    src/main.mm
    src/capture/mac_monitors.mm
    src/capture/mac_capture.mm
    src/encode/vt_encoder.mm
    src/input/mac_input.mm
    src/stream/mac_desktop_stream.mm
    src/session/mac_session.mm
    src/server/mac_stream_server.mm
)

add_executable(rincast-streamer-mac ${MAC_STREAMER_SOURCES})

target_compile_options(rincast-streamer-mac PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    -Wall -Wextra -Wno-unused-parameter
)

target_include_directories(rincast-streamer-mac PRIVATE
    src
    ${RC_SHARED}/include
    ${RC_THIRD_PARTY}
    ${RC_THIRD_PARTY}/nlohmann
)

target_link_libraries(rincast-streamer-mac PRIVATE
    rincast-core
    "-framework Cocoa"
    "-framework CoreGraphics"
    "-framework ScreenCaptureKit"
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework ApplicationServices"
    "-framework Carbon"
)
