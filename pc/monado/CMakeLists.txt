# RinCast's Monado service (pc/monado/README.md): Monado's OpenXR runtime and service, built from
# Monado's own sources (pinned below, fetched at configure time, unmodified), with RinCast's devices
# and compositor target in place of Monado's hardware drivers. Linux and Windows.
#
#   cmake -S pc/monado -B build/monado -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/monado
#
# Everything ends up in build/monado/monado: rincast-monado (the service), Monado's OpenXR runtime
# library and rincast-monado.json, its OpenXR manifest. The streamer looks for that folder next to
# itself (installed) or in the build tree.
cmake_minimum_required(VERSION 3.22)
# A version for Monado's libmonado, which takes the top-level project's.
project(rincast-monado VERSION 25.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(RC_MONADO_OUT ${CMAKE_BINARY_DIR}/monado)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${RC_MONADO_OUT})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${RC_MONADO_OUT})
foreach(config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${config} CONFIG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG} ${RC_MONADO_OUT})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG} ${RC_MONADO_OUT})
endforeach()

# ---- Monado: only what a streamed headset needs ----
# Names of its own, so it never meets a Monado the user installed: its socket / pipe, pid file and
# runtime library. On Windows the runtime starts the service by this name when a game opens it.
set(XRT_IPC_MSG_SOCK_FILENAME rincast_monado_ipc CACHE STRING "" FORCE)
set(XRT_IPC_SERVICE_PID_FILENAME rincast-monado.pid CACHE STRING "" FORCE)
set(XRT_OXR_RUNTIME_SUFFIX rincast_monado CACHE STRING "" FORCE)
set(XRT_SERVICE_EXECUTABLE rincast-monado CACHE STRING "" FORCE)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(XRT_FEATURE_OPENXR ON CACHE BOOL "" FORCE)
set(XRT_FEATURE_SERVICE ON CACHE BOOL "" FORCE)
foreach(off
    XRT_FEATURE_SERVICE_SYSTEMD XRT_INSTALL_SYSTEMD_UNIT_FILES XRT_INSTALL_ABSOLUTE_SYSTEMD_UNIT_FILES
    XRT_FEATURE_STEAMVR_PLUGIN XRT_FEATURE_DEBUG_GUI XRT_FEATURE_CLIENT_DEBUG_GUI XRT_FEATURE_WINDOW_PEEK
    XRT_FEATURE_TRACING XRT_FEATURE_SLAM XRT_FEATURE_RENDERDOC
    XRT_MODULE_MONADO_CLI XRT_MODULE_MONADO_GUI XRT_MODULE_AUX_VIVE XRT_MODULE_MERCURY_HANDTRACKING
    XRT_BUILD_SAMPLES XRT_HAVE_SDL2 XRT_HAVE_OPENCV XRT_HAVE_TRACY XRT_HAVE_PERCETTO XRT_HAVE_GST
    XRT_HAVE_REALSENSE XRT_HAVE_ONNXRUNTIME XRT_HAVE_LIBUVC XRT_HAVE_JPEG XRT_HAVE_BLUETOOTH XRT_HAVE_OPENVR
    XRT_BUILD_DRIVER_ANDROID XRT_BUILD_DRIVER_ARDUINO XRT_BUILD_DRIVER_BLUBUR_S1 XRT_BUILD_DRIVER_DAYDREAM
    XRT_BUILD_DRIVER_DEPTHAI XRT_BUILD_DRIVER_EUROC XRT_BUILD_DRIVER_HANDTRACKING XRT_BUILD_DRIVER_TWRAP
    XRT_BUILD_DRIVER_HDK XRT_BUILD_DRIVER_HYDRA XRT_BUILD_DRIVER_ILLIXR XRT_BUILD_DRIVER_NS XRT_BUILD_DRIVER_OHMD
    XRT_BUILD_DRIVER_OPENGLOVES XRT_BUILD_DRIVER_PSMV XRT_BUILD_DRIVER_PSSENSE XRT_BUILD_DRIVER_PSVR
    XRT_BUILD_DRIVER_QWERTY XRT_BUILD_DRIVER_REALSENSE XRT_BUILD_DRIVER_REMOTE XRT_BUILD_DRIVER_RIFT
    XRT_BUILD_DRIVER_RIFT_S XRT_BUILD_DRIVER_ROKID XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE XRT_BUILD_DRIVER_SURVIVE
    XRT_BUILD_DRIVER_ULV2 XRT_BUILD_DRIVER_ULV5 XRT_BUILD_DRIVER_VF XRT_BUILD_DRIVER_VIVE XRT_BUILD_DRIVER_WMR
    XRT_BUILD_DRIVER_XREAL_AIR XRT_BUILD_DRIVER_SIMULAVR XRT_BUILD_DRIVER_SOLARXR XRT_BUILD_DRIVER_SIMULATED)
  set(${off} OFF CACHE BOOL "" FORCE)
endforeach()

include(FetchContent)
# Monado 25.1.0.
FetchContent_Declare(monado
  GIT_REPOSITORY https://gitlab.freedesktop.org/monado/monado.git
  GIT_TAG 3b450e37b624f5edea366492ee2d267d673a35aa
  GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(monado)

find_package(Vulkan REQUIRED)

# ---- the service ----
add_executable(rincast-monado
  src/main.cpp
  src/hub.cpp
  src/devices.cpp
  src/target.cpp
  src/instance.cpp
)
target_include_directories(rincast-monado PRIVATE src ${RC_ROOT}/pc/common ${RC_ROOT}/shared/include)
target_link_libraries(rincast-monado PRIVATE
  ipc_server ipc_shared comp_main comp_util aux_vk aux_util aux_os aux_math xrt-interfaces Vulkan::Vulkan)
if(WIN32)
  target_compile_definitions(rincast-monado PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
add_dependencies(rincast-monado openxr_${XRT_OXR_RUNTIME_SUFFIX})

# The OpenXR manifest, next to the runtime library it names (a path relative to the manifest).
if(WIN32)
  set(RC_RUNTIME_LIBRARY openxr_${XRT_OXR_RUNTIME_SUFFIX}.dll)
else()
  set(RC_RUNTIME_LIBRARY libopenxr_${XRT_OXR_RUNTIME_SUFFIX}.so)
endif()
file(WRITE ${RC_MONADO_OUT}/rincast-monado.json
"{
    \"file_format_version\": \"1.0.0\",
    \"runtime\": {
        \"name\": \"RinCast (Monado)\",
        \"library_path\": \"./${RC_RUNTIME_LIBRARY}\"
    }
}
")
