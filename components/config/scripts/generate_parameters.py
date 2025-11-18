#!/usr/bin/env python3
"""
Parameter Code Generator for QRS2HST Keyer

Reads parameters.yaml and generates:
  - parameter_table.hpp: PARAMETER_TABLE macro for NVS storage
  - parameter_registry_generated.cpp: RegisterAllParameters() with lambdas

Usage:
  python3 generate_parameters.py --input parameters.yaml \\
                                  --output-table include/config/parameter_table.hpp \\
                                  --output-registry parameter_registry_generated.cpp

Author: Feature 4 - Parameter Metadata Unification
"""

import argparse
import sys
import yaml
import json
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Any, Optional

# Try to import jsonschema for validation
try:
    from jsonschema import validate, ValidationError
    JSONSCHEMA_AVAILABLE = True
except ImportError:
    JSONSCHEMA_AVAILABLE = False
    print("Warning: jsonschema not available - YAML validation will be limited", file=sys.stderr)


def load_and_validate_yaml(yaml_path: Path, schema_path: Optional[Path] = None) -> Dict[str, Any]:
    """
    Load parameters.yaml and validate against JSON Schema.

    Args:
        yaml_path: Path to parameters.yaml
        schema_path: Path to parameters_schema.json (optional)

    Returns:
        Parsed YAML data as dictionary

    Raises:
        SystemExit: If YAML is invalid or validation fails
    """
    # Load YAML
    try:
        with open(yaml_path, 'r') as f:
            data = yaml.safe_load(f)
    except yaml.YAMLError as e:
        print(f"Error parsing YAML file {yaml_path}:", file=sys.stderr)
        print(f"  {e}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print(f"Error: YAML file not found: {yaml_path}", file=sys.stderr)
        sys.exit(1)

    # Basic structure check
    if not isinstance(data, dict) or 'parameters' not in data:
        print(f"Error: YAML must have top-level 'parameters' key", file=sys.stderr)
        sys.exit(1)

    if not isinstance(data['parameters'], list):
        print(f"Error: 'parameters' must be an array", file=sys.stderr)
        sys.exit(1)

    if len(data['parameters']) == 0:
        print(f"Error: 'parameters' array is empty", file=sys.stderr)
        sys.exit(1)

    # JSON Schema validation (if available and schema provided)
    if JSONSCHEMA_AVAILABLE and schema_path and schema_path.exists():
        try:
            with open(schema_path, 'r') as f:
                schema = json.load(f)
            validate(instance=data, schema=schema)
            print(f"✅ YAML validation passed ({len(data['parameters'])} parameters)", file=sys.stderr)
        except ValidationError as e:
            print(f"Error: YAML validation failed:", file=sys.stderr)
            print(f"  {e.message}", file=sys.stderr)
            if e.path:
                print(f"  Path: parameters[{'.'.join(str(p) for p in e.path)}]", file=sys.stderr)
            sys.exit(1)
    elif schema_path and not schema_path.exists():
        print(f"Warning: Schema file not found: {schema_path}", file=sys.stderr)

    # Manual validation of required fields
    for i, param in enumerate(data['parameters']):
        required_fields = ['subsystem', 'name', 'nvs_key', 'field', 'type',
                          'reset_required', 'description', 'unit', 'validator']

        for field in required_fields:
            if field not in param:
                print(f"Error in parameters[{i}]: Missing required field '{field}'", file=sys.stderr)
                print(f"  Parameter: {param.get('subsystem', '?')}.{param.get('name', '?')}", file=sys.stderr)
                sys.exit(1)

        # Type-specific validation
        param_type = param['type']
        if param_type in ['INT32', 'UINT32', 'UINT16', 'UINT8', 'INT8', 'FLOAT']:
            if 'min' not in param or 'max' not in param:
                print(f"Error in parameters[{i}]: {param_type} requires 'min' and 'max' fields", file=sys.stderr)
                print(f"  Parameter: {param['subsystem']}.{param['name']}", file=sys.stderr)
                sys.exit(1)
        elif param_type == 'ENUM':
            if 'enum_type' not in param or 'values' not in param:
                print(f"Error in parameters[{i}]: ENUM requires 'enum_type' and 'values' fields", file=sys.stderr)
                print(f"  Parameter: {param['subsystem']}.{param['name']}", file=sys.stderr)
                sys.exit(1)

    return data


def generate_parameter_table_hpp(parameters: List[Dict[str, Any]], output_path: Path) -> None:
    """
    Generate parameter_table.hpp with complete type definitions and PARAMETER_TABLE macro.

    Args:
        parameters: List of parameter dictionaries from YAML
        output_path: Path to write parameter_table.hpp
    """
    lines = []

    # File header
    lines.append("// Auto-generated from parameters.yaml")
    lines.append("// DO NOT EDIT - Changes will be overwritten on build!")
    lines.append(f"// Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"// Generator: generate_parameters.py")
    lines.append(f"// {len(parameters)} parameters total")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cctype>     // for std::toupper")
    lines.append("#include <cstddef>")
    lines.append("#include <cstdint>")
    lines.append("#include <cstring>")
    lines.append("#include <functional>")
    lines.append("#include <type_traits>")
    lines.append("#include <utility>  // for std::declval")
    lines.append('#include "config/device_config.hpp"')
    lines.append("")
    lines.append("namespace config {")
    lines.append("")

    # NvsType enum
    lines.append("//")
    lines.append("// 1. NVS Type Enumeration")
    lines.append("//")
    lines.append("")
    lines.append("enum class NvsType {")
    lines.append("  INT32,   // int32_t")
    lines.append("  UINT32,  // uint32_t")
    lines.append("  INT16,   // int16_t (unused currently)")
    lines.append("  UINT16,  // uint16_t")
    lines.append("  INT8,    // int8_t")
    lines.append("  UINT8,   // uint8_t")
    lines.append("  BOOL,    // bool (stored as uint8_t: 0 = false, 1 = true)")
    lines.append("  STRING,  // char[] null-terminated")
    lines.append("  FLOAT    // float (unused currently, reserved)")
    lines.append("};")
    lines.append("")

    # ValidatorFunc typedef
    lines.append("//")
    lines.append("// 2. Validator Function Type")
    lines.append("//")
    lines.append("")
    lines.append("using ValidatorFunc = std::function<bool(const void*, const DeviceConfig&)>;")
    lines.append("")

    # ParameterDescriptor struct
    lines.append("//")
    lines.append("// 3. Parameter Descriptor Structure")
    lines.append("//")
    lines.append("")
    lines.append("struct ParameterDescriptor {")
    lines.append("  const char* name;           // Dot-separated name: \"audio.freq\"")
    lines.append("  const char* nvs_key;        // NVS key: \"audio_freq\"")
    lines.append("  NvsType type;               // NvsType::UINT16")
    lines.append("  size_t offset;              // offsetof(DeviceConfig, audio.sidetone_frequency_hz)")
    lines.append("  size_t size;                // sizeof(uint16_t)")
    lines.append("  ValidatorFunc validator;    // Range/GPIO/String validator")
    lines.append("  bool requires_reset;        // true if hardware param (needs reboot)")
    lines.append("  const char* description;    // Human-readable description")
    lines.append("  const char* unit;           // Unit string: \"Hz\", \"WPM\", \"%\", \"\"")
    lines.append("};")
    lines.append("")

    # Validator implementations
    lines.append("//")
    lines.append("// 4. Validator Implementations")
    lines.append("//")
    lines.append("")
    lines.append("template <typename T>")
    lines.append("struct RangeValidator {")
    lines.append("  T min;")
    lines.append("  T max;")
    lines.append("")
    lines.append("  bool operator()(const void* value_ptr, const DeviceConfig& /* cfg */) const {")
    lines.append("    T value = *static_cast<const T*>(value_ptr);")
    lines.append("    return value >= min && value <= max;")
    lines.append("  }")
    lines.append("};")
    lines.append("")
    lines.append("struct GpioUniqueValidator {")
    lines.append("  int32_t min;")
    lines.append("  int32_t max;")
    lines.append("")
    lines.append("  bool operator()(const void* value_ptr, const DeviceConfig& cfg) const {")
    lines.append("    int32_t gpio = *static_cast<const int32_t*>(value_ptr);")
    lines.append("    if (gpio == -1) return true;  // -1 is valid (disabled)")
    lines.append("    if (gpio < min || gpio > max) return false;")
    lines.append("    return true;")
    lines.append("  }")
    lines.append("};")
    lines.append("")
    lines.append("struct StringCallsignValidator {")
    lines.append("  bool operator()(const void* str_ptr, const DeviceConfig& /* cfg */) const {")
    lines.append("    const char* str = static_cast<const char*>(str_ptr);")
    lines.append("    for (const char* p = str; *p; ++p) {")
    lines.append("      const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));")
    lines.append("      const bool valid = (upper >= 'A' && upper <= 'Z') ||")
    lines.append("                         (upper >= '0' && upper <= '9') ||")
    lines.append("                         upper == '/' || upper == '-';")
    lines.append("      if (!valid) return false;")
    lines.append("    }")
    lines.append("    return true;")
    lines.append("  }")
    lines.append("};")
    lines.append("")
    lines.append("struct StringPrintableValidator {")
    lines.append("  bool operator()(const void* str_ptr, const DeviceConfig& /* cfg */) const {")
    lines.append("    const char* str = static_cast<const char*>(str_ptr);")
    lines.append("    for (const char* p = str; *p; ++p) {")
    lines.append("      const unsigned char uch = static_cast<unsigned char>(*p);")
    lines.append("      if (uch < 32 || uch > 126) return false;")
    lines.append("    }")
    lines.append("    return true;")
    lines.append("  }")
    lines.append("};")
    lines.append("")

    # Validator tags
    lines.append("//")
    lines.append("// 5. Validator Type Tags (for macro dispatch)")
    lines.append("//")
    lines.append("")
    lines.append("struct RangeValidatorTag {};")
    lines.append("struct GpioUniqueValidatorTag {};")
    lines.append("struct StringCallsignValidatorTag {};")
    lines.append("struct StringPrintableValidatorTag {};")
    lines.append("")

    # Validator factory functions
    lines.append("//")
    lines.append("// 6. Validator Factory Helper")
    lines.append("//")
    lines.append("")
    lines.append("template <typename FieldType, typename ValidatorTag>")
    lines.append("inline ValidatorFunc MakeValidator(int32_t min, int32_t max);")
    lines.append("")
    lines.append("template <typename FieldType>")
    lines.append("inline ValidatorFunc MakeValidator(int32_t min, int32_t max, RangeValidatorTag) {")
    lines.append("  return RangeValidator<FieldType>{static_cast<FieldType>(min), static_cast<FieldType>(max)};")
    lines.append("}")
    lines.append("")
    lines.append("template <typename FieldType>")
    lines.append("inline ValidatorFunc MakeValidator(int32_t min, int32_t max, GpioUniqueValidatorTag) {")
    lines.append("  return GpioUniqueValidator{min, max};")
    lines.append("}")
    lines.append("")
    lines.append("template <typename FieldType>")
    lines.append("inline ValidatorFunc MakeValidator(int32_t /* min */, int32_t /* max */, StringCallsignValidatorTag) {")
    lines.append("  return StringCallsignValidator{};")
    lines.append("}")
    lines.append("")
    lines.append("template <typename FieldType>")
    lines.append("inline ValidatorFunc MakeValidator(int32_t /* min */, int32_t /* max */, StringPrintableValidatorTag) {")
    lines.append("  return StringPrintableValidator{};")
    lines.append("}")
    lines.append("")

    # PARAMETER_TABLE macro
    lines.append("//")
    lines.append("// 7. PARAMETER_TABLE Macro (Single Source of Truth)")
    lines.append("//")
    lines.append("")
    lines.append("// PARAMETER_TABLE macro for code generation")
    lines.append("// Usage: PARAMETER_TABLE(X) where X is a macro that processes each parameter")
    lines.append("//")
    lines.append("// Macro arguments:")
    lines.append("//   subsystem  - Parameter subsystem (audio, keying, hardware, wifi, general)")
    lines.append("//   param      - Parameter short name")
    lines.append("//   nvs_key    - NVS storage key (max 15 chars)")
    lines.append("//   field      - DeviceConfig field path")
    lines.append("//   type       - NVS type (INT32, UINT32, UINT16, UINT8, INT8, BOOL, STRING, FLOAT, ENUM)")
    lines.append("//   min        - Minimum value (for numeric types)")
    lines.append("//   max        - Maximum value (for numeric types)")
    lines.append("//   reset      - Reset required flag (true/false)")
    lines.append("//   desc       - Short description string")
    lines.append("//   unit       - Unit string (e.g., \"Hz\", \"WPM\", \"%\")")
    lines.append("//   validator  - Validator tag")
    lines.append("#define PARAMETER_TABLE(X) \\")

    # Generate X() invocations for each parameter
    for i, param in enumerate(parameters):
        subsystem = param['subsystem']
        name = param['name']
        nvs_key = param['nvs_key']
        field = param['field']
        param_type = param['type']

        # ENUM parameters are stored in NVS as their underlying type (UINT8)
        # The parameter metadata system uses EnumParameter<T>, but NVS uses the raw value
        nvs_type = 'UINT8' if param_type == 'ENUM' else param_type

        # For ENUM types, compute min/max from enum values (need numeric values from C++ enum)
        # For now, use 0-255 as safe range for UINT8 enums
        if param_type == 'ENUM':
            min_val = 0
            max_val = 255
        else:
            min_val = param.get('min', 0)
            max_val = param.get('max', 0)
        reset = 'true' if param['reset_required'] else 'false'
        desc = param['description'].replace('"', '\\"')  # Escape quotes
        unit = param['unit']
        validator = param['validator']

        # Last parameter doesn't need trailing backslash
        trailing = " \\" if i < len(parameters) - 1 else ""

        lines.append(f'  X({subsystem}, {name}, "{nvs_key}", {field}, {nvs_type}, {min_val}, {max_val}, {reset}, "{desc}", "{unit}", {validator}){trailing}')

    lines.append("")

    # Global constants
    lines.append("//")
    lines.append("// 8. Global Constants")
    lines.append("//")
    lines.append("")
    lines.append("// Parameter count")
    lines.append(f"constexpr size_t kParameterCount = {len(parameters)};")
    lines.append("")
    lines.append("// Parameter descriptor array (defined in parameter_table.cpp)")
    lines.append("extern const ParameterDescriptor kParameterDescriptors[kParameterCount];")
    lines.append("")

    lines.append("}  // namespace config")
    lines.append("")

    # Write file
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, 'w') as f:
        f.write('\n'.join(lines))

    print(f"✅ Generated {output_path} ({len(lines)} lines)", file=sys.stderr)


def generate_parameter_registry_cpp(parameters: List[Dict[str, Any]], output_path: Path) -> None:
    """
    Generate parameter_registry_generated.cpp with RegisterAllParameters().

    Args:
        parameters: List of parameter dictionaries from YAML
        output_path: Path to write parameter_registry_generated.cpp
    """
    lines = []

    # File header
    lines.append("// Auto-generated from parameters.yaml")
    lines.append("// DO NOT EDIT - Changes will be overwritten on build!")
    lines.append(f"// Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"// Generator: generate_parameters.py")
    lines.append("")
    lines.append("#include \"config/parameter_registry.hpp\"")
    lines.append("#include \"config/parameter_metadata.hpp\"")
    lines.append("#include \"config/device_config.hpp\"")
    lines.append("#include \"config/keying_presets.hpp\"")
    lines.append("")
    lines.append("#include <memory>")
    lines.append("#include <vector>")
    lines.append("#include <cstring>")
    lines.append("")
    lines.append("namespace config {")
    lines.append("")
    lines.append("void RegisterAllParameters(ParameterRegistry& registry) {")
    lines.append(f"  // Auto-generated parameter registrations from YAML ({len(parameters)} parameters)")
    lines.append("")

    # Generate registrations for each parameter
    for param in parameters:
        subsystem = param['subsystem']
        name = param['name']
        param_name = f"{subsystem}.{name}"
        field = param['field']
        param_type = param['type']
        desc = param['description'].replace('"', '\\"')
        unit = param['unit']

        lines.append(f"  // {param_name}")

        if param_type in ['INT32', 'UINT32', 'UINT16', 'UINT8']:
            min_val = param['min']
            max_val = param['max']
            lines.append(f"  registry.Register(std::make_unique<IntParameter<{min_val}, {max_val}>>(")
            lines.append(f"      \"{param_name}\", \"{desc}\", \"{unit}\",")
            lines.append(f"      [](const DeviceConfig& c) -> int32_t {{ return static_cast<int32_t>(c.{field}); }},")
            lines.append(f"      [](DeviceConfig& c, int32_t v) {{ c.{field} = static_cast<decltype(c.{field})>(v); }}")
            lines.append(f"  ));")

        elif param_type == 'INT8':
            min_val = param['min']
            max_val = param['max']
            lines.append(f"  registry.Register(std::make_unique<IntParameter<{min_val}, {max_val}>>(")
            lines.append(f"      \"{param_name}\", \"{desc}\", \"{unit}\",")
            lines.append(f"      [](const DeviceConfig& c) -> int32_t {{ return static_cast<int32_t>(c.{field}); }},")
            lines.append(f"      [](DeviceConfig& c, int32_t v) {{ c.{field} = static_cast<int8_t>(v); }}")
            lines.append(f"  ));")

        elif param_type == 'BOOL':
            lines.append(f"  registry.Register(std::make_unique<BooleanParameter>(")
            lines.append(f"      \"{param_name}\", \"{desc}\",")
            lines.append(f"      \"true\", \"false\",")
            lines.append(f"      [](const DeviceConfig& c) -> bool {{ return c.{field}; }},")
            lines.append(f"      [](DeviceConfig& c, bool v) {{ c.{field} = v; }}")
            lines.append(f"  ));")

        elif param_type == 'STRING':
            min_len = param.get('min', 0)
            max_len = param.get('max', 255)
            lines.append(f"  registry.Register(std::make_unique<StringParameter>(")
            lines.append(f"      \"{param_name}\", \"{desc}\", {min_len}, {max_len},")
            lines.append(f"      [](const DeviceConfig& c) -> std::string {{ return std::string(c.{field}); }},")
            lines.append(f"      [](DeviceConfig& c, std::string_view v) {{")
            lines.append(f"        const size_t copy_len = (v.size() < sizeof(c.{field}) - 1) ? v.size() : (sizeof(c.{field}) - 1);")
            lines.append(f"        std::memset(c.{field}, 0, sizeof(c.{field}));")
            lines.append(f"        std::memcpy(c.{field}, v.data(), copy_len);")
            lines.append(f"      }}")
            lines.append(f"  ));")

        elif param_type == 'FLOAT':
            min_val = param['min']
            max_val = param['max']
            precision = param.get('precision', 1)
            lines.append(f"  registry.Register(std::make_unique<FloatParameter<{precision}>>(")
            lines.append(f"      \"{param_name}\", \"{desc}\", \"{unit}\",")
            lines.append(f"      static_cast<float>({min_val}), static_cast<float>({max_val}),")
            lines.append(f"      [](const DeviceConfig& c) -> float {{ return c.{field}; }},")
            lines.append(f"      [](DeviceConfig& c, float v) {{ c.{field} = v; }}")
            lines.append(f"  ));")

        elif param_type == 'ENUM':
            enum_type = param['enum_type']
            values = param['values']
            lines.append(f"  registry.Register(std::make_unique<EnumParameter<{enum_type}>>(")
            lines.append(f"      \"{param_name}\", \"{desc}\",")
            lines.append(f"      std::vector<EnumParameter<{enum_type}>::EnumValue>{{")
            for val in values:
                val_name = val['name']
                val_value = val['value']
                val_desc = val['description'].replace('"', '\\"')
                lines.append(f"        {{ {enum_type}::{val_value}, \"{val_name}\", \"{val_desc}\" }},")
            lines.append(f"      }},")
            lines.append(f"      [](const DeviceConfig& c) -> {enum_type} {{ return c.{field}; }},")
            lines.append(f"      [](DeviceConfig& c, {enum_type} v) {{ c.{field} = v; }}")
            lines.append(f"  ));")

        else:
            print(f"Warning: Unsupported type {param_type} for {param_name}", file=sys.stderr)

        # Add SetCategory() call if category is specified in YAML
        if 'category' in param:
            category = param['category']
            # Use temporary variable to set category on the parameter
            lines.append(f"  registry.Find(\"{param_name}\")->SetCategory(\"{category}\");")

        lines.append("")

    lines.append("}")
    lines.append("")
    lines.append("}  // namespace config")
    lines.append("")

    # Write file
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, 'w') as f:
        f.write('\n'.join(lines))

    print(f"✅ Generated {output_path} ({len(lines)} lines)", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(
        description="Generate C++ parameter metadata code from YAML",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Example:
  python3 generate_parameters.py \\
      --input parameters.yaml \\
      --output-table include/config/parameter_table.hpp \\
      --output-registry parameter_registry_generated.cpp
        """
    )

    parser.add_argument('--input', required=True, type=Path,
                       help='Input YAML file (parameters.yaml)')
    parser.add_argument('--output-table', required=True, type=Path,
                       help='Output HPP file for PARAMETER_TABLE macro')
    parser.add_argument('--output-registry', required=True, type=Path,
                       help='Output CPP file for RegisterAllParameters()')
    parser.add_argument('--schema', type=Path,
                       help='JSON Schema file for validation (optional)')

    args = parser.parse_args()

    # Load and validate YAML
    print(f"Loading {args.input}...", file=sys.stderr)
    data = load_and_validate_yaml(args.input, args.schema)
    parameters = data['parameters']

    # Generate files
    print(f"Generating {args.output_table}...", file=sys.stderr)
    generate_parameter_table_hpp(parameters, args.output_table)

    print(f"Generating {args.output_registry}...", file=sys.stderr)
    generate_parameter_registry_cpp(parameters, args.output_registry)

    print(f"✅ Code generation complete: {len(parameters)} parameters", file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
