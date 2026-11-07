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
  app/strings.cpp
  gfx/gl.cpp
  gfx/scene.cpp
  gfx/sky.cpp
  gfx/ui_renderer.cpp
  media/audio.cpp
  media/music.cpp
  media/video_decoder.cpp
  media/video_decoder_common.cpp
  net/cloud.cpp
  net/stream_client.cpp
  platform/assets.cpp
  platform/jni_bridge.cpp
  ui/ui.cpp
  ui/window.cpp
  xr/xr_context.cpp
  xr/xr_loader.cpp
  xr/xr_input.cpp
  # What differs between headsets; see headsets/headset.h.
  headsets/headset.cpp
  headsets/quest/quest_headset.cpp
  headsets/quest/quest_body.cpp
  headsets/quest/quest_face.cpp
  headsets/pico/pico_headset.cpp
  headsets/pico/pico_body.cpp
  headsets/pico/pico_face.cpp
  headsets/androidxr/androidxr_headset.cpp
  stb_impl.cpp
)
# VERSION is the single source of the version, as for the PC side.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RC_ROOT}/VERSION")
file(READ "${RC_ROOT}/VERSION" RINCAST_VERSION)
string(STRIP "${RINCAST_VERSION}" RINCAST_VERSION)
target_compile_definitions(rincast PRIVATE RINCAST_VERSION_STRING="${RINCAST_VERSION}")
target_compile_options(rincast PRIVATE -Wall -Wno-unused-parameter -Wno-missing-field-initializers -fno-strict-aliasing)
target_include_directories(rincast PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${RC_ROOT}/third_party)
# The glue's entry point must not be stripped.
set_target_properties(rincast PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
target_link_libraries(rincast PRIVATE
  rincast-core
  native_app_glue
  android log EGL GLESv3 mediandk aaudio nativewindow jnigraphics
)

# The OpenXR loader isn't linked: the app opens one when it starts (xr/xr_loader.cpp), Khronos'
# standard loader or, on PICO headsets from before PICO OS 5.9, PICO's own. Both go next to
# librincast.so, so they're packaged with it. Only the headers come from the Khronos package.
get_target_property(RC_OPENXR_LOADER OpenXR::openxr_loader IMPORTED_LOCATION)
target_link_libraries(rincast PRIVATE OpenXR::headers)

# PICO's loader comes from PICO's OpenXR SDK, downloaded once and checked against this hash.
set(RC_PICO_SDK_URL "https://sdk.picovr.com/developer-platform/sdk/PICO_OpenXR_SDK_220.zip")
set(RC_PICO_SDK_SHA256 2524927c1b791521c5b7bb518878de0a428793f2d91d60b7a3d84bb7eb6f1e9c)
set(RC_PICO_SDK_ZIP "${RC_ROOT}/deps/pico/PICO_OpenXR_SDK_220.zip")
if(NOT EXISTS "${RC_PICO_SDK_ZIP}")
  message(STATUS "Downloading PICO's OpenXR loader")
  file(DOWNLOAD "${RC_PICO_SDK_URL}" "${RC_PICO_SDK_ZIP}.part" EXPECTED_HASH SHA256=${RC_PICO_SDK_SHA256} TLS_VERIFY ON)
  file(RENAME "${RC_PICO_SDK_ZIP}.part" "${RC_PICO_SDK_ZIP}")
endif()
file(SHA256 "${RC_PICO_SDK_ZIP}" RC_PICO_SDK_ACTUAL)
if(NOT RC_PICO_SDK_ACTUAL STREQUAL RC_PICO_SDK_SHA256)
  message(FATAL_ERROR "${RC_PICO_SDK_ZIP} isn't the expected file. Delete it to download it again.")
endif()
set(RC_PICO_DIR "${CMAKE_CURRENT_BINARY_DIR}/pico-openxr")
file(ARCHIVE_EXTRACT INPUT "${RC_PICO_SDK_ZIP}" DESTINATION "${RC_PICO_DIR}" PATTERNS "libs/android.${ANDROID_ABI}/libopenxr_loader.so")
# The Quest 1 build also carries Meta's loader, from Meta's OpenXR Mobile SDK 64 (Oculus SDK
# License), downloaded once and checked the same way.
option(RINCAST_QUEST1 "Also package Meta's OpenXR loader for the Quest 1" OFF)
if(RINCAST_QUEST1)
  set(RC_META_SDK_URL "https://securecdn.oculus.com/binaries/download/?id=7577210995650755")
  set(RC_META_SDK_SHA256 c9b5e9ee5effcc93d1bf3763a0a83789d83385cfad803744b3f37f08bcd1bf7c)
  set(RC_META_SDK_ZIP "${RC_ROOT}/deps/meta/ovr_openxr_mobile_sdk_64.zip")
  if(NOT EXISTS "${RC_META_SDK_ZIP}")
    message(STATUS "Downloading Meta's OpenXR loader for the Quest 1")
    file(DOWNLOAD "${RC_META_SDK_URL}" "${RC_META_SDK_ZIP}.part" EXPECTED_HASH SHA256=${RC_META_SDK_SHA256} TLS_VERIFY ON)
    file(RENAME "${RC_META_SDK_ZIP}.part" "${RC_META_SDK_ZIP}")
  endif()
  file(SHA256 "${RC_META_SDK_ZIP}" RC_META_SDK_ACTUAL)
  if(NOT RC_META_SDK_ACTUAL STREQUAL RC_META_SDK_SHA256)
    message(FATAL_ERROR "${RC_META_SDK_ZIP} isn't the expected file. Delete it to download it again.")
  endif()
  set(RC_META_DIR "${CMAKE_CURRENT_BINARY_DIR}/meta-openxr")
  file(ARCHIVE_EXTRACT INPUT "${RC_META_SDK_ZIP}" DESTINATION "${RC_META_DIR}"
       PATTERNS "OpenXR/Libs/Android/${ANDROID_ABI}/Release/libopenxr_loader.so")
  add_custom_command(TARGET rincast POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RC_META_DIR}/OpenXR/Libs/Android/${ANDROID_ABI}/Release/libopenxr_loader.so"
            "$<TARGET_FILE_DIR:rincast>/libopenxr_loader_quest1.so")
endif()
add_custom_command(TARGET rincast POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RC_OPENXR_LOADER}" "$<TARGET_FILE_DIR:rincast>/libopenxr_loader.so"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RC_PICO_DIR}/libs/android.${ANDROID_ABI}/libopenxr_loader.so"
          "$<TARGET_FILE_DIR:rincast>/libopenxr_loader_pico.so")
