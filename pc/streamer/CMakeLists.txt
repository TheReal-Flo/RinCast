# RinCast Streamer (Windows desktop app)
set(RC_TP ${CMAKE_CURRENT_LIST_DIR}/../../third_party)

add_executable(rincast-streamer WIN32
  src/main.cpp
  src/app.cpp
  src/net/firewall.cpp
  src/util.cpp
  src/settings.cpp
  src/gpu.cpp
  src/capture/monitors.cpp
  src/capture/desktop_capture.cpp
  src/encode/encoder.cpp
  src/encode/color_convert.cpp
  src/encode/nvenc_encoder.cpp
  src/encode/amf_encoder.cpp
  src/encode/mf_encoder.cpp
  src/stream/video_sender.cpp
  src/stream/desktop_stream.cpp
  src/stream/vr_stream.cpp
  src/stream/motion_estimator.cpp
  src/net/stream_server.cpp
  src/net/port_mapper.cpp
  src/net/http.cpp
  src/session/client_session.cpp
  src/input/input_injector.cpp
  src/audio/audio.cpp
  src/vr/vr_bridge_host.cpp
  src/vr/face_share.cpp
  src/vr/steamvr.cpp
  src/games/game_library.cpp
  src/cloud/cloud_client.cpp
  src/resource.rc
)
set_target_properties(rincast-streamer PROPERTIES OUTPUT_NAME "RinCast Streamer")
target_include_directories(rincast-streamer PRIVATE
  src
  ../common
  ${RC_TP}
  ${RC_TP}/webview2/include
)
target_compile_definitions(rincast-streamer PRIVATE UNICODE _UNICODE)
target_link_libraries(rincast-streamer PRIVATE
  rincast-core
  ${RC_TP}/webview2/lib/WebView2LoaderStatic.lib
  version
)
if(MSVC)
  target_compile_options(rincast-streamer PRIVATE /W3 /utf-8 /permissive- /Zc:__cplusplus /MP)
  target_link_options(rincast-streamer PRIVATE /MANIFEST:NO)
endif()

# Ship the web UI next to the executable.
add_custom_command(TARGET rincast-streamer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_LIST_DIR}/ui $<TARGET_FILE_DIR:rincast-streamer>/ui
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/../../assets/brand/mark-256.png $<TARGET_FILE_DIR:rincast-streamer>/ui/mark.png
)
