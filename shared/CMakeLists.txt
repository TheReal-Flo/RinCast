# rincast-core: protocol, transport, crypto and audio codec shared by the
# PC streamer and the headset app.
cmake_minimum_required(VERSION 3.22)

set(RC_THIRD_PARTY ${CMAKE_CURRENT_LIST_DIR}/../third_party)

set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
if(NOT TARGET opus)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  if(MSVC AND DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
    set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(${RC_THIRD_PARTY}/opus ${CMAKE_CURRENT_BINARY_DIR}/opus EXCLUDE_FROM_ALL)
endif()

add_library(rincast-core STATIC
  src/platform.cpp
  src/socket.cpp
  src/crypto.cpp
  src/connection.cpp
  src/handshake.cpp
  src/video_transport.cpp
  src/audio_codec.cpp
  src/music.cpp
  src/discovery.cpp
  ${RC_THIRD_PARTY}/monocypher/monocypher.c
)
target_include_directories(rincast-core PUBLIC
  include
  ${RC_THIRD_PARTY}
  ${RC_THIRD_PARTY}/monocypher
)
target_compile_features(rincast-core PUBLIC cxx_std_20)
target_link_libraries(rincast-core PUBLIC opus)
if(WIN32)
  target_compile_definitions(rincast-core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_link_libraries(rincast-core PUBLIC ws2_32 iphlpapi bcrypt)
elseif(ANDROID)
  target_link_libraries(rincast-core PUBLIC log)
endif()

option(RINCAST_CORE_TESTS "Build rincast-core tests" OFF)
if(RINCAST_CORE_TESTS)
  add_executable(rincast-core-tests tests/core_tests.cpp)
  target_link_libraries(rincast-core-tests PRIVATE rincast-core)
endif()
