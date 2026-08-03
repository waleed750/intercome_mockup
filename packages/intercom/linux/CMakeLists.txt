cmake_minimum_required(VERSION 3.10)
set(PROJECT_NAME "intercom")
project(${PROJECT_NAME} LANGUAGES CXX)

set(PLUGIN_NAME "intercom_plugin")

add_library(${PLUGIN_NAME} SHARED
  "intercom_plugin.cc"
  "video_decoder_handler.cc"
  "audio_pipeline_handler.cc"
  "ringer_handler.cc"
)
apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(PkgConfig REQUIRED)
# gst-plugins-base ships gstreamer-app-1.0 (appsrc/appsink); the actual
# decode/encode elements used at runtime (h264parse, avdec_h264, alawenc,
# alawdec, audiobuffersplit) come from gst-plugins-good/bad/ugly/libav and
# are resolved by name at pipeline-parse time, not linked here - see the
# apt package list in the package README's Linux section.
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0>=1.14
  gstreamer-app-1.0>=1.14
)

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GST)

# List of absolute paths to libraries that should be bundled with the plugin
set(intercom_bundled_libraries
  ""
  PARENT_SCOPE
)
