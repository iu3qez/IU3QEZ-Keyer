cmake_minimum_required(VERSION 3.16)
project(host_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HOST_TEST_COVERAGE "Enable coverage instrumentation for host tests" OFF)

if(HOST_TEST_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "Coverage instrumentation requires GCC or Clang")
  endif()
  message(STATUS "Host test coverage instrumentation enabled")
  # Disable optimizations for accurate coverage accounting and enable profiling.
  add_compile_options(-O0 -g)
  set(COVERAGE_COMPILE_FLAGS --coverage -fprofile-arcs -ftest-coverage)
  set(COVERAGE_LINK_FLAGS --coverage)
endif()
enable_testing()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)

FetchContent_MakeAvailable(googletest)

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

add_library(esp_idf_stubs STATIC
  stubs/esp_idf_stubs.cpp
)
target_include_directories(esp_idf_stubs
  PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/stubs
    ${CMAKE_CURRENT_LIST_DIR}/support
)

add_library(firmware_components STATIC
  ${REPO_ROOT}/components/keyer_hal/high_precision_clock.cpp
  ${REPO_ROOT}/components/keyer_hal/paddle_hal.cpp
  ${REPO_ROOT}/components/keying/paddle_engine.cpp
  ${REPO_ROOT}/components/config/storage.cpp
  ${REPO_ROOT}/components/config/parameter_registry.cpp
  ${REPO_ROOT}/components/config/parameter_registry_generated.cpp
  ${REPO_ROOT}/components/config/parameter_table.cpp
  ${REPO_ROOT}/components/morse_decoder/adaptive_timing_classifier.cpp
  ${REPO_ROOT}/components/morse_decoder/morse_table.cpp
  ${REPO_ROOT}/components/morse_decoder/morse_decoder.cpp
  stubs/cJSON.cpp
)
target_include_directories(firmware_components
  PUBLIC
    ${REPO_ROOT}/components/keyer_hal/include
    ${REPO_ROOT}/components/keying/include
    ${REPO_ROOT}/components/config/include
    ${REPO_ROOT}/components/timeline/include
    ${REPO_ROOT}/components/morse_decoder/include
    ${REPO_ROOT}/components/app/include
    ${REPO_ROOT}/components/diagnostics_subsystem/include
    ${REPO_ROOT}/components/keying_subsystem/include
    ${CMAKE_CURRENT_LIST_DIR}/support
    ${CMAKE_CURRENT_LIST_DIR}/stubs
    /opt/esp/idf/components/json/cJSON
)
target_compile_definitions(firmware_components
  PUBLIC
    HAL_USE_LED_STRIP_STUB=1
)
target_link_libraries(firmware_components
  PUBLIC
    esp_idf_stubs
)

add_executable(all_host_tests
  high_precision_clock_test.cpp
  paddle_hal_test.cpp
  paddle_engine_test.cpp
  # status_led_test.cpp - removed after LED refactoring to diagnostics_subsystem
  storage_test.cpp
  parameter_metadata_test.cpp
  sidetone_service_test.cpp
  audio_stream_player_test.cpp
  # init_pipeline_test.cpp - temporarily disabled (requires FreeRTOS dependencies)
  test_adaptive_timing_classifier.cpp
  test_morse_table.cpp
  test_morse_decoder.cpp
  support/fake_codec_factory.cpp
  ${REPO_ROOT}/components/audio_subsystem/sidetone_service.cpp
  ${REPO_ROOT}/components/audio_subsystem/tone_generator.cpp
  ${REPO_ROOT}/components/audio_subsystem/audio_stream_player.cpp
)
target_include_directories(all_host_tests
  PRIVATE
    ${REPO_ROOT}/components/timeline/include
    ${REPO_ROOT}/components/audio_subsystem/include
    ${REPO_ROOT}/components/app/include
    ${CMAKE_CURRENT_LIST_DIR}/support
    ${CMAKE_CURRENT_LIST_DIR}/stubs
    /opt/esp/idf/components/json/cJSON
)
target_compile_definitions(all_host_tests
  PRIVATE
    HAL_USE_LED_STRIP_STUB=1
)
target_link_libraries(all_host_tests
  PRIVATE
    firmware_components
    esp_idf_stubs
    GTest::gtest_main
)

if(HOST_TEST_COVERAGE)
  target_compile_options(esp_idf_stubs PRIVATE ${COVERAGE_COMPILE_FLAGS})
  target_compile_options(firmware_components PRIVATE ${COVERAGE_COMPILE_FLAGS})
  target_compile_options(all_host_tests PRIVATE ${COVERAGE_COMPILE_FLAGS})
  target_link_options(all_host_tests PRIVATE ${COVERAGE_LINK_FLAGS})
endif()

include(GoogleTest)
gtest_discover_tests(all_host_tests)
