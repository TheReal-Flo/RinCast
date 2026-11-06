# RinCast Virtual Audio (kernel-mode WaveRT/KMDF driver).
#
# Built with plain MSVC + the WDK (NuGet package Microsoft.Windows.WDK.x64 or
# an installed WDK). Configure with:
#   cmake -S pc/audio-driver -B build/audio -A x64 -DWDK_ROOT=<path to WDK "c" folder or Windows Kits\10>
cmake_minimum_required(VERSION 3.22)
project(RinCastAudio LANGUAGES CXX)

set(WDK_ROOT "" CACHE PATH "WDK root (folder containing Include/ and Lib/)")
set(WDK_VERSION "" CACHE STRING "WDK version folder, e.g. 10.0.28000.0 (auto-detected)")
set(KMDF_VERSION "1.15" CACHE STRING "KMDF version to build against")

if(NOT WDK_ROOT)
  message(FATAL_ERROR "Set -DWDK_ROOT=... (see scripts/fetch-deps.ps1)")
endif()
if(NOT WDK_VERSION)
  file(GLOB _km_dirs LIST_DIRECTORIES true "${WDK_ROOT}/Include/10.*")
  foreach(d ${_km_dirs})
    if(EXISTS "${d}/km/portcls.h")
      get_filename_component(WDK_VERSION ${d} NAME)
    endif()
  endforeach()
endif()
message(STATUS "WDK ${WDK_VERSION} at ${WDK_ROOT}")

set(INC ${WDK_ROOT}/Include/${WDK_VERSION})
set(LIB ${WDK_ROOT}/Lib/${WDK_VERSION}/km/x64)
string(REPLACE "." ";" _kv ${KMDF_VERSION})
list(GET _kv 0 KMDF_MAJOR)
list(GET _kv 1 KMDF_MINOR)

file(GLOB SOURCES src/Main/*.cpp src/Utilities/*.cpp src/Filters/*.cpp)
add_executable(rincastaudio ${SOURCES} src/Main/rincastaudio.rc)
set_target_properties(rincastaudio PROPERTIES SUFFIX ".sys" MSVC_RUNTIME_LIBRARY "")

target_include_directories(rincastaudio SYSTEM PRIVATE
  ${INC}/km ${INC}/km/crt ${INC}/shared
  ${WDK_ROOT}/Include/wdf/kmdf/${KMDF_VERSION})
target_include_directories(rincastaudio PRIVATE src src/Inc src/Filters src/Main src/Utilities)
target_compile_definitions(rincastaudio PRIVATE
  _AMD64_ _WIN64 AMD64 POOL_NX_OPTIN=1 POOL_ZERO_DOWN_LEVEL_SUPPORT=1
  NTDDI_VERSION=0x0A00000C _WIN32_WINNT=0x0A00 WINVER=0x0A00 WINNT=1
  _WIN32 UNICODE _UNICODE PC_IMPLEMENTATION DEBUG_LEVEL=DEBUGLVL_TERSE _NEW_DELETE_OPERATORS_
  KMDF_VERSION_MAJOR=${KMDF_MAJOR} KMDF_VERSION_MINOR=${KMDF_MINOR}
  $<$<CONFIG:Debug>:DBG=1>)
# Kernel-mode code generation.
target_compile_options(rincastaudio PRIVATE
  /kernel /GS /Gy /Zp8 /GF /Gz /Zc:wchar_t- /Zc:forScope /Zc:inline /GR- /EHs-c- /W4 /wd4201 /wd4214 /wd4627
  /d1import_no_registry /d2AllowCompatibleILVersions /d2Zi+ /Zi /cbstring /FIwarning.h
  $<$<CONFIG:Release>:/O2 /Oi /Oy->)
string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
target_link_options(rincastaudio PRIVATE
  /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:FxDriverEntry /NODEFAULTLIB /MANIFEST:NO
  /INCREMENTAL:NO /OPT:REF /OPT:ICF /MERGE:_TEXT=.text /MERGE:_PAGE=PAGE /SECTION:INIT,d
  /IGNORE:4198,4010,4037,4039,4065,4070,4078,4087,4089,4221 /DEBUG /RELEASE /INTEGRITYCHECK)
target_link_libraries(rincastaudio PRIVATE
  ${LIB}/BufferOverflowFastFailK.lib ${LIB}/ntoskrnl.lib ${LIB}/hal.lib ${LIB}/wmilib.lib
  ${LIB}/portcls.lib ${LIB}/stdunk.lib ${LIB}/libcntpr.lib ${LIB}/ks.lib ${LIB}/ksguid.lib
  ${WDK_ROOT}/Lib/wdf/kmdf/x64/${KMDF_VERSION}/WdfDriverEntry.lib
  ${WDK_ROOT}/Lib/wdf/kmdf/x64/${KMDF_VERSION}/WdfLdr.lib)
