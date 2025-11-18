find_package(Python3 REQUIRED COMPONENTS Interpreter)
idf_build_get_property(PROJECT_DIR project_dir)

set(WEBUI_ROOT "${project_dir}/webui")
set(WEBUI_DIST "${WEBUI_ROOT}/dist")
set(WEBUI_SCRIPT "${project_dir}/scripts/webui/embed_assets.py")

# Check if npm is available
find_program(NPM_EXECUTABLE npm)
if(NOT NPM_EXECUTABLE)
    message(WARNING "npm not found - WebUI assets will not be rebuilt automatically")
endif()

set(WEBUI_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(WEBUI_GENERATED_INC "${WEBUI_GENERATED_DIR}/web_assets_data.inc")
file(MAKE_DIRECTORY "${WEBUI_GENERATED_DIR}")

# Log tags generation
set(LOG_TAGS_SCRIPT "${project_dir}/tools/generate_log_tags.py")
set(LOG_TAGS_GENERATED_HEADER "${WEBUI_GENERATED_DIR}/generated_log_tags.hpp")
set(COMPONENTS_DIR "${project_dir}/components")
# Collect all .cpp and .hpp files for dependency tracking
file(GLOB_RECURSE COMPONENT_SOURCE_FILES
     "${COMPONENTS_DIR}/*.cpp"
     "${COMPONENTS_DIR}/*.hpp")

idf_component_register(
  SRCS
    "http_server.cpp"
    "web_assets.cpp"
    "serial_console.cpp"
    "console_commands.cpp"
    "console_parameter_bridge.cpp"
    "console_system_commands.cpp"
    "InputHandler.cpp"
    "CommandDispatcher.cpp"
    "OutputBuffer.cpp"
  INCLUDE_DIRS
    "include"
  PRIV_INCLUDE_DIRS
    "${WEBUI_GENERATED_DIR}"
  REQUIRES
    esp_http_server
    config
    wifi_subsystem
    remote
    json
    espressif__esp_tinyusb
  PRIV_REQUIRES
    app
    timeline
    app_update
    system_monitor
    text_keyer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

# Build webui/dist/ if npm is available and build script exists
set(WEBUI_BUILD_SCRIPT "${WEBUI_ROOT}/build.sh")
if(NPM_EXECUTABLE AND EXISTS "${WEBUI_BUILD_SCRIPT}")
    # Source files for dependency tracking
    file(GLOB_RECURSE WEBUI_SOURCE_FILES
         "${WEBUI_ROOT}/src/*.ts"
         "${WEBUI_ROOT}/src/*.svelte"
         "${WEBUI_ROOT}/src/*.css"
         "${WEBUI_ROOT}/package.json"
         "${WEBUI_ROOT}/vite.config.ts"
         "${WEBUI_ROOT}/tsconfig.json")

    add_custom_command(
      OUTPUT "${WEBUI_DIST}/index.html"
      COMMAND bash "${WEBUI_BUILD_SCRIPT}"
      WORKING_DIRECTORY "${WEBUI_ROOT}"
      DEPENDS ${WEBUI_SOURCE_FILES} "${WEBUI_BUILD_SCRIPT}"
      COMMENT "Building WebUI (build.sh)"
      VERBATIM)

    add_custom_target(webui_build_target DEPENDS "${WEBUI_DIST}/index.html")
elseif(NOT EXISTS "${WEBUI_DIST}/index.html")
    message(WARNING "WebUI dist/ not found and npm/build script unavailable. Run 'cd webui && npm run build' manually.")
endif()

# Asset files for dependency tracking (Svelte SPA built to dist/)
file(GLOB_RECURSE WEBUI_ASSET_FILES
     "${WEBUI_DIST}/*.html"
     "${WEBUI_DIST}/*.js"
     "${WEBUI_DIST}/*.css"
     "${WEBUI_ROOT}/css/*.css")

add_custom_command(
  OUTPUT "${WEBUI_GENERATED_INC}"
  COMMAND ${Python3_EXECUTABLE} "${WEBUI_SCRIPT}"
          --source "${WEBUI_ROOT}"
          --output "${WEBUI_GENERATED_INC}"
          --gzip
  DEPENDS ${WEBUI_ASSET_FILES} "${WEBUI_SCRIPT}"
  COMMENT "Embedding Web UI assets (Svelte SPA)"
  VERBATIM)

add_custom_target(webui_assets_target ALL DEPENDS "${WEBUI_GENERATED_INC}")

# If npm is available, make webui_assets depend on webui build
if(NPM_EXECUTABLE)
    add_dependencies(webui_assets_target webui_build_target)
endif()

add_dependencies(${COMPONENT_LIB} webui_assets_target)

set_source_files_properties(web_assets.cpp PROPERTIES OBJECT_DEPENDS "${WEBUI_GENERATED_INC}")

# Generate logging tags header
add_custom_command(
  OUTPUT "${LOG_TAGS_GENERATED_HEADER}"
  COMMAND ${Python3_EXECUTABLE} "${LOG_TAGS_SCRIPT}"
          "${COMPONENTS_DIR}"
          "${LOG_TAGS_GENERATED_HEADER}"
  DEPENDS ${COMPONENT_SOURCE_FILES} "${LOG_TAGS_SCRIPT}"
  COMMENT "Auto-generating logging tags list from components"
  VERBATIM)

add_custom_target(log_tags_target ALL DEPENDS "${LOG_TAGS_GENERATED_HEADER}")
add_dependencies(${COMPONENT_LIB} log_tags_target)

set_source_files_properties(console_system_commands.cpp PROPERTIES OBJECT_DEPENDS "${LOG_TAGS_GENERATED_HEADER}")
