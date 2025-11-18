# Register component (must be first in ESP-IDF)
idf_component_register(SRCS "storage.cpp"
                            "keying_presets.cpp"
                            "parameter_registry.cpp"
                            "parameter_table.cpp"
                            "parameter_registry_generated.cpp"  # Generated file (Task 4.3)
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash driver json)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

# Find Python 3 for parameter code generation (Feature 4 - Task 4.3)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Define paths for generated files
set(PARAM_YAML "${CMAKE_CURRENT_SOURCE_DIR}/parameters.yaml")
set(PARAM_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/parameters_schema.json")
set(GEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_parameters.py")
set(GEN_TABLE_HPP "${CMAKE_CURRENT_SOURCE_DIR}/include/config/parameter_table.hpp")
set(GEN_REGISTRY_CPP "${CMAKE_CURRENT_SOURCE_DIR}/parameter_registry_generated.cpp")

# Generate parameter code from YAML before build
# This runs automatically when parameters.yaml or the script changes
add_custom_command(
    OUTPUT
        ${GEN_TABLE_HPP}
        ${GEN_REGISTRY_CPP}
    COMMAND
        ${Python3_EXECUTABLE} ${GEN_SCRIPT}
        --input ${PARAM_YAML}
        --output-table ${GEN_TABLE_HPP}
        --output-registry ${GEN_REGISTRY_CPP}
        --schema ${PARAM_SCHEMA}
    DEPENDS
        ${PARAM_YAML}
        ${GEN_SCRIPT}
        ${PARAM_SCHEMA}
    COMMENT
        "Generating parameter files from YAML"
    VERBATIM
)

# Create custom target for generation
add_custom_target(generate_parameters_target ALL
    DEPENDS
        ${GEN_TABLE_HPP}
        ${GEN_REGISTRY_CPP}
)

# Ensure generation happens before compiling component
add_dependencies(${COMPONENT_LIB} generate_parameters_target)
