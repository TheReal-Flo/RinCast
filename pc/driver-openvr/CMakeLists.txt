# RinCast SteamVR driver (driver_rincast.dll) laid out as a SteamVR driver folder:
#   <bin>/driver/rincast/driver.vrdrivermanifest
#   <bin>/driver/rincast/resources/...
#   <bin>/driver/rincast/bin/win64/driver_rincast.dll
add_library(driver_rincast SHARED src/driver.cpp)
target_include_directories(driver_rincast PRIVATE ../common ${CMAKE_CURRENT_LIST_DIR}/../../third_party)
target_compile_definitions(driver_rincast PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  target_compile_options(driver_rincast PRIVATE /W3 /utf-8 /permissive-)
endif()
set(RINCAST_DRIVER_DIR ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/driver/rincast)
set_target_properties(driver_rincast PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release/driver/rincast/bin/win64
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/Debug/driver/rincast/bin/win64
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/bin/RelWithDebInfo/driver/rincast/bin/win64)
add_custom_command(TARGET driver_rincast POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_LIST_DIR}/package/rincast ${RINCAST_DRIVER_DIR})
