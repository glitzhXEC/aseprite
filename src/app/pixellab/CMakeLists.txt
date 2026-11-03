# Experimental in-process client; enable explicitly in the Aseprite build.
option(PIXELLAB_FAKE_NETWORK_FOR_TESTS "Use synthetic offline data; never enable in production" OFF)
include(dependencies.cmake)
if(PIXELLAB_FAKE_NETWORK_FOR_TESTS)
  set(PIXELLAB_CLIENT_SOURCE tests/mock_adapter.cpp)
else()
  set(PIXELLAB_CLIENT_SOURCE client.cpp)
endif()
add_library(pixellab-native-client STATIC
  ${PIXELLAB_CLIENT_SOURCE}
  ${PIXELLAB_DEPS_DIR}/vendor/miniz/miniz.c
  ${PIXELLAB_DEPS_DIR}/vendor/miniz/miniz_tdef.c
  ${PIXELLAB_DEPS_DIR}/vendor/miniz/miniz_tinfl.c
  ${PIXELLAB_DEPS_DIR}/vendor/miniz/miniz_zip.c)
target_compile_features(pixellab-native-client PUBLIC cxx_std_17)
if(WIN32)
  target_link_libraries(pixellab-native-client PRIVATE winhttp)
elseif(UNIX AND NOT APPLE)
  find_package(CURL REQUIRED)
  target_link_libraries(pixellab-native-client PRIVATE CURL::libcurl)
else()
  message(FATAL_ERROR "PixelLab native client is not ported to this platform")
endif()
target_include_directories(pixellab-native-client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PIXELLAB_DEPS_DIR})
