#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include "cJSON.h"
}
#include "config/device_config.hpp"

namespace config {

/**
 * @brief Visibility condition function type
 *
 * Returns true if the parameter should be visible/accessible given the current config.
 * Used to implement context-dependent parameters that only appear in specific modes.
 *
 * PATTERN FOR CONTEXT-DEPENDENT PARAMETERS (Task 5.7.6):
 * ========================================================
 *
 * 1. Create parameter with std::make_unique<>
 * 2. Call SetVisibilityCondition() with lambda checking config state
 * 3. Register parameter with registry.Register(std::move(param))
 *
 * Example 1: Keying window parameters (only visible when preset == MANUAL)
 * @code
 *   auto window_param = std::make_unique<FloatParameter<1>>(
 *       "keying.window_open", "Memory window open threshold", "%",
 *       0.0f, 100.0f, getter, setter);
 *   window_param->SetVisibilityCondition([](const DeviceConfig& cfg) {
 *     return cfg.keying.preset == KeyingPreset::kManual;
 *   });
 *   registry.Register(std::move(window_param));
 * @endcode
 *
 * Example 2: WiFi password (only visible when WiFi enabled)
 * @code
 *   auto wifi_pass = std::make_unique<StringParameter>(
 *       "wifi.password", "WiFi password", "", getter, setter);
 *   wifi_pass->SetVisibilityCondition([](const DeviceConfig& cfg) {
 *     return cfg.wifi.enabled;
 *   });
 *   registry.Register(std::move(wifi_pass));
 * @endcode
 *
 * Example 3: Advanced audio params (only visible when audio enabled)
 * @code
 *   auto fade_in = std::make_unique<IntParameter<0, 100>>(
 *       "audio.fade_in", "Fade-in time", "ms", getter, setter);
 *   fade_in->SetVisibilityCondition([](const DeviceConfig& cfg) {
 *     return cfg.audio.sidetone_enabled;
 *   });
 *   registry.Register(std::move(fade_in));
 * @endcode
 *
 * VISIBILITY ENFORCEMENT:
 * - GetVisibleParameters() filters by visibility in registry queries
 * - ExportJsonSchema() excludes invisible parameters from Web UI
 * - GenerateHelpText() only shows visible parameters in console help
 * - Console commands can check param->IsVisible() before execution
 *
 * DESIGN RATIONALE:
 * - Prevents user confusion (hide irrelevant parameters)
 * - Reduces cognitive load in help text and Web UI
 * - Enforces mode-specific parameter sets (e.g., MANUAL vs preset modes)
 * - Supports dynamic UI generation based on configuration state
 */
using VisibilityCondition = std::function<bool(const DeviceConfig&)>;

/**
 * @brief Abstract base class for configuration parameters
 *
 * Provides a unified interface for:
 * - Type-safe parameter validation
 * - Execution (parsing and applying values to DeviceConfig)
 * - Current value retrieval for status displays
 * - JSON schema export for Web UI auto-generation
 * - Conditional visibility based on config state
 *
 * Subclasses implement specific types (int, float, enum, bool) with appropriate
 * validation rules and conversion logic.
 */
class Parameter {
 public:
  /**
   * @brief Construct a parameter with metadata
   *
   * @param name Parameter name (e.g., "audio.freq", "keying.wpm")
   * @param description Human-readable description for help text
   * @param unit Unit string (e.g., "Hz", "WPM", "%") or empty
   */
  Parameter(const char* name, const char* description, const char* unit = "")
      : name_(name), description_(description), unit_(unit), requires_reset_(false), category_(nullptr) {}

  virtual ~Parameter() = default;

  // Non-copyable (parameters are registered in unique_ptr containers)
  Parameter(const Parameter&) = delete;
  Parameter& operator=(const Parameter&) = delete;

  //
  // Accessors
  //

  /** @brief Get parameter name (subsystem.param format) */
  const char* GetName() const { return name_; }

  /** @brief Get human-readable description */
  const char* GetDescription() const { return description_; }

  /** @brief Get unit string (empty if dimensionless) */
  const char* GetUnit() const { return unit_; }

  /**
   * @brief Set parameter category override
   *
   * @param category "normal" or "advanced" - overrides auto-detection
   */
  void SetCategory(const char* category) {
    category_ = category;
  }

  /**
   * @brief Get parameter category for UI organization
   *
   * @return "normal" for user-facing params, "advanced" for hardware/low-level params
   */
  const char* GetCategory() const {
    // Use explicit category if set
    if (category_ != nullptr) {
      return category_;
    }

    // Fallback: determine category based on parameter name patterns
    // Advanced: hardware pins, I2C/I2S, IO expander, timing parameters
    const char* n = name_;
    if (strstr(n, "_gpio") || strstr(n, "_addr") || strstr(n, "timing_") ||
        strstr(n, "ioexp_") || strstr(n, "_scl") || strstr(n, "_sda") ||
        strstr(n, "_mclk") || strstr(n, "_bclk") || strstr(n, "_lrck") ||
        strstr(n, "_dout") || strstr(n, "i2c_") || strstr(n, "i2s_")) {
      return "advanced";
    }
    return "normal";
  }

  //
  // Visibility control
  //

  /**
   * @brief Set conditional visibility predicate
   *
   * @param condition Function that returns true when parameter is visible/accessible
   *
   * Example:
   * @code
   *   param->SetVisibilityCondition([](const DeviceConfig& cfg) {
   *     return cfg.keying.preset == KeyingPreset::kManual;
   *   });
   * @endcode
   */
  void SetVisibilityCondition(VisibilityCondition condition) {
    visibility_condition_ = std::move(condition);
  }

  /**
   * @brief Check if parameter is visible given current config
   *
   * @param config Current device configuration
   * @return true if parameter should be shown/accessible, false otherwise
   */
  bool IsVisible(const DeviceConfig& config) const {
    return !visibility_condition_ || visibility_condition_(config);
  }

  //
  // Reset requirement control
  //

  /**
   * @brief Mark parameter as requiring device reset when changed
   *
   * Hardware parameters (GPIO pins, I2C/I2S configuration) require device reset
   * to take effect. This flag is used to display warnings in the Web UI.
   *
   * @param requires_reset true if parameter change requires reset, false otherwise
   *
   * Example:
   * @code
   *   auto gpio_param = std::make_unique<IntParameter<0, 48>>(
   *       "paddle.dit_gpio", "Dit paddle GPIO pin", "", getter, setter);
   *   gpio_param->SetRequiresReset(true);  // Hardware change - needs reset
   *   registry.Register(std::move(gpio_param));
   * @endcode
   */
  void SetRequiresReset(bool requires_reset) {
    requires_reset_ = requires_reset;
  }

  /**
   * @brief Check if parameter change requires device reset
   *
   * @return true if changing this parameter requires device reset, false otherwise
   */
  bool GetRequiresReset() const {
    return requires_reset_;
  }

  //
  // Type and validation interface (pure virtual)
  //

  /**
   * @brief Get parameter type name ("int", "float", "enum", "bool")
   *
   * Used for help text and JSON schema generation.
   */
  virtual const char* GetTypeName() const = 0;

  /**
   * @brief Get range/values description for help text (optional)
   *
   * @return String describing valid range or values (e.g., "0-100", "on/off", "V0-V9/MANUAL")
   *         Returns empty string if not applicable.
   *
   * Used by GenerateHelpText() to show constraints.
   */
  virtual std::string GetRangeDescription() const { return ""; }

  /**
   * @brief Validate parameter value string
   *
   * @param value_str Value string to validate (e.g., "700", "on", "V3")
   * @param error Optional output for validation error message
   * @return true if value_str is valid, false otherwise
   */
  virtual bool Validate(const char* value_str, std::string* error) const = 0;

  //
  // Execution interface (pure virtual)
  //

  /**
   * @brief Parse value string, update config, return result message
   *
   * @param value_str Value string to parse and apply
   * @param config Configuration to update (modified in-place on success)
   * @param result_message Optional output for result message ("OK ..." or "ERR ...")
   * @return true on success, false on validation failure
   *
   * Example result messages:
   * - "OK freq=700Hz"
   * - "ERR Value must be between 100 and 2000"
   */
  virtual bool Execute(const char* value_str, DeviceConfig& config,
                       std::string* result_message) = 0;

  //
  // Current value retrieval (pure virtual)
  //

  /**
   * @brief Get current value as string for status displays
   *
   * @param config Current device configuration
   * @return String representation of current value (e.g., "700", "on", "V3")
   */
  virtual std::string GetCurrentValue(const DeviceConfig& config) const = 0;

  //
  // JSON schema export (pure virtual)
  //

  /**
   * @brief Create a JSON schema object describing the parameter
   *
   * Implementations return a newly allocated cJSON object containing metadata used
   * by the Web UI (type, widget hints, ranges, etc.). Callers take ownership and
   * must release it with cJSON_Delete(). Returning nullptr signals allocation failure.
   */
  virtual cJSON* CreateJsonSchema() const = 0;

 private:
  const char* name_;
  const char* description_;
  const char* unit_;
  VisibilityCondition visibility_condition_;
  bool requires_reset_;
  const char* category_;  // nullptr = auto-detect, otherwise "normal" or "advanced"
};

//
// Specialized parameter templates
//

/**
 * @brief Integer parameter with compile-time min/max bounds
 *
 * @tparam MIN Minimum allowed value (inclusive)
 * @tparam MAX Maximum allowed value (inclusive)
 *
 * Example:
 * @code
 *   // Sidetone frequency 100-2000 Hz
 *   IntParameter<100, 2000> freq_param(
 *       "audio.freq",
 *       "Sidetone frequency",
 *       "Hz",
 *       [](const DeviceConfig& cfg) { return cfg.audio.sidetone_frequency_hz; },
 *       [](DeviceConfig& cfg, int32_t val) { cfg.audio.sidetone_frequency_hz = val; }
 *   );
 * @endcode
 */
template <int32_t MIN, int32_t MAX>
class IntParameter : public Parameter {
 public:
  using Getter = std::function<int32_t(const DeviceConfig&)>;
  using Setter = std::function<void(DeviceConfig&, int32_t)>;

  IntParameter(const char* name, const char* description, const char* unit,
               Getter getter, Setter setter)
      : Parameter(name, description, unit), getter_(std::move(getter)), setter_(std::move(setter)) {}

  const char* GetTypeName() const override { return "int"; }

  std::string GetRangeDescription() const override {
    char buf[64];
    snprintf(buf, sizeof(buf), "%ld-%ld", static_cast<long>(MIN), static_cast<long>(MAX));
    return buf;
  }

  bool Validate(const char* value_str, std::string* error) const override {
    char* endptr = nullptr;
    const long value = std::strtol(value_str, &endptr, 10);

    if (endptr == value_str || *endptr != '\0') {
      if (error) *error = "Invalid integer format";
      return false;
    }

    if (value < MIN || value > MAX) {
      if (error) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Value must be between %ld and %ld",
                 static_cast<long>(MIN), static_cast<long>(MAX));
        *error = buf;
      }
      return false;
    }

    return true;
  }

  bool Execute(const char* value_str, DeviceConfig& config,
               std::string* result_message) override {
    std::string error;
    if (!Validate(value_str, &error)) {
      if (result_message) *result_message = "ERR " + error;
      return false;
    }

    const int32_t value = static_cast<int32_t>(std::strtol(value_str, nullptr, 10));
    setter_(config, value);

    if (result_message) {
      char buf[128];
      snprintf(buf, sizeof(buf), "OK %s=%ld%s", GetName(),
               static_cast<long>(value), GetUnit());
      *result_message = buf;
    }

    return true;
  }

  std::string GetCurrentValue(const DeviceConfig& config) const override {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", static_cast<long>(getter_(config)));
    return buf;
  }

  cJSON* CreateJsonSchema() const override {
    cJSON* schema = cJSON_CreateObject();
    if (schema == nullptr) {
      return nullptr;
    }

    if (cJSON_AddStringToObject(schema, "name", GetName()) == nullptr ||
        cJSON_AddStringToObject(schema, "type", "int") == nullptr ||
        cJSON_AddStringToObject(schema, "widget", "number_input") == nullptr ||
        cJSON_AddNumberToObject(schema, "min", static_cast<double>(MIN)) == nullptr ||
        cJSON_AddNumberToObject(schema, "max", static_cast<double>(MAX)) == nullptr ||
        cJSON_AddStringToObject(schema, "unit", GetUnit()) == nullptr ||
        cJSON_AddStringToObject(schema, "description", GetDescription()) == nullptr ||
        cJSON_AddStringToObject(schema, "category", GetCategory()) == nullptr) {
      cJSON_Delete(schema);
      return nullptr;
    }

    return schema;
  }

 private:
  Getter getter_;
  Setter setter_;
};

/**
 * @brief Floating-point parameter with range validation
 *
 * @tparam PRECISION Number of decimal places for display (default 1)
 *
 * Example:
 * @code
 *   // Memory window percentage 0.0-100.0%
 *   FloatParameter<1> window_param(
 *       "keying.window",
 *       "Memory window open percentage",
 *       "%",
 *       0.0f, 100.0f,
 *       [](const DeviceConfig& cfg) { return cfg.keying.memory_open_percent; },
 *       [](DeviceConfig& cfg, float val) { cfg.keying.memory_open_percent = val; }
 *   );
 * @endcode
 */
template <int PRECISION = 1>
class FloatParameter : public Parameter {
 public:
  using Getter = std::function<float(const DeviceConfig&)>;
  using Setter = std::function<void(DeviceConfig&, float)>;

  FloatParameter(const char* name, const char* description, const char* unit,
                 float min, float max, Getter getter, Setter setter)
      : Parameter(name, description, unit),
        min_(min), max_(max), getter_(std::move(getter)), setter_(std::move(setter)) {}

  const char* GetTypeName() const override { return "float"; }

  std::string GetRangeDescription() const override {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f-%.1f", min_, max_);
    return buf;
  }

  bool Validate(const char* value_str, std::string* error) const override {
    char* endptr = nullptr;
    const float value = std::strtof(value_str, &endptr);

    if (endptr == value_str || *endptr != '\0') {
      if (error) *error = "Invalid float format";
      return false;
    }

    if (value < min_ || value > max_) {
      if (error) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Value must be between %.1f and %.1f", min_, max_);
        *error = buf;
      }
      return false;
    }

    return true;
  }

  bool Execute(const char* value_str, DeviceConfig& config,
               std::string* result_message) override {
    std::string error;
    if (!Validate(value_str, &error)) {
      if (result_message) *result_message = "ERR " + error;
      return false;
    }

    const float value = std::strtof(value_str, nullptr);
    setter_(config, value);

    if (result_message) {
      char buf[128];
      snprintf(buf, sizeof(buf), "OK %s=%.*f%s", GetName(), PRECISION, value, GetUnit());
      *result_message = buf;
    }

    return true;
  }

  std::string GetCurrentValue(const DeviceConfig& config) const override {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", PRECISION, getter_(config));
    return buf;
  }

  cJSON* CreateJsonSchema() const override {
    cJSON* schema = cJSON_CreateObject();
    if (schema == nullptr) {
      return nullptr;
    }

    if (cJSON_AddStringToObject(schema, "name", GetName()) == nullptr ||
        cJSON_AddStringToObject(schema, "type", "float") == nullptr ||
        cJSON_AddStringToObject(schema, "widget", "slider") == nullptr ||
        cJSON_AddNumberToObject(schema, "min", static_cast<double>(min_)) == nullptr ||
        cJSON_AddNumberToObject(schema, "max", static_cast<double>(max_)) == nullptr ||
        cJSON_AddNumberToObject(schema, "precision", static_cast<double>(PRECISION)) == nullptr ||
        cJSON_AddStringToObject(schema, "unit", GetUnit()) == nullptr ||
        cJSON_AddStringToObject(schema, "description", GetDescription()) == nullptr ||
        cJSON_AddStringToObject(schema, "category", GetCategory()) == nullptr) {
      cJSON_Delete(schema);
      return nullptr;
    }

    return schema;
  }

 private:
  float min_;
  float max_;
  Getter getter_;
  Setter setter_;
};

/**
 * @brief Enumeration parameter with allowed values
 *
 * @tparam EnumType Enum type (e.g., KeyingPreset)
 *
 * Example:
 * @code
 *   EnumParameter<KeyingPreset>::EnumValue preset_values[] = {
 *       {KeyingPreset::kAccukeyerBoth, "V3", "Accukeyer with dot+dash memory"},
 *       {KeyingPreset::kCurtisABoth, "V6", "Curtis A with dot+dash memory"},
 *       // ...
 *   };
 *   EnumParameter<KeyingPreset> preset_param(
 *       "keying.preset",
 *       "Iambic keying preset",
 *       {preset_values, preset_values + sizeof(preset_values)/sizeof(preset_values[0])},
 *       [](const DeviceConfig& cfg) { return cfg.keying.preset; },
 *       [](DeviceConfig& cfg, KeyingPreset val) { cfg.keying.preset = val; }
 *   );
 * @endcode
 */
template <typename EnumType>
class EnumParameter : public Parameter {
 public:
  struct EnumValue {
    EnumType value;
    const char* name;
    const char* description;
  };

  using Getter = std::function<EnumType(const DeviceConfig&)>;
  using Setter = std::function<void(DeviceConfig&, EnumType)>;

  EnumParameter(const char* name, const char* description,
                const std::vector<EnumValue>& allowed_values,
                Getter getter, Setter setter)
      : Parameter(name, description, ""),
        allowed_values_(allowed_values), getter_(std::move(getter)), setter_(std::move(setter)) {}

  const char* GetTypeName() const override { return "enum"; }

  std::string GetRangeDescription() const override {
    std::string result;
    for (size_t i = 0; i < allowed_values_.size(); ++i) {
      if (i > 0) result += "/";
      result += allowed_values_[i].name;
    }
    return result;
  }

  bool Validate(const char* value_str, std::string* error) const override {
    for (const auto& av : allowed_values_) {
      if (strcasecmp(value_str, av.name) == 0) {
        return true;
      }
    }

    if (error) {
      std::string allowed = "Allowed values: ";
      for (size_t i = 0; i < allowed_values_.size(); ++i) {
        if (i > 0) allowed += ", ";
        allowed += allowed_values_[i].name;
      }
      *error = allowed;
    }

    return false;
  }

  bool Execute(const char* value_str, DeviceConfig& config,
               std::string* result_message) override {
    for (const auto& av : allowed_values_) {
      if (strcasecmp(value_str, av.name) == 0) {
        setter_(config, av.value);

        if (result_message) {
          char buf[128];
          snprintf(buf, sizeof(buf), "OK %s=%s", GetName(), av.name);
          *result_message = buf;
        }

        return true;
      }
    }

    std::string error;
    Validate(value_str, &error);
    if (result_message) *result_message = "ERR " + error;
    return false;
  }

  std::string GetCurrentValue(const DeviceConfig& config) const override {
    const EnumType current = getter_(config);
    for (const auto& av : allowed_values_) {
      if (av.value == current) {
        return av.name;
      }
    }
    return "<unknown>";
  }

  cJSON* CreateJsonSchema() const override {
    cJSON* schema = cJSON_CreateObject();
    if (schema == nullptr) {
      return nullptr;
    }

    if (cJSON_AddStringToObject(schema, "name", GetName()) == nullptr ||
        cJSON_AddStringToObject(schema, "type", "enum") == nullptr ||
        cJSON_AddStringToObject(schema, "widget", "dropdown") == nullptr ||
        cJSON_AddStringToObject(schema, "description", GetDescription()) == nullptr ||
        cJSON_AddStringToObject(schema, "category", GetCategory()) == nullptr) {
      cJSON_Delete(schema);
      return nullptr;
    }

    cJSON* values = cJSON_AddArrayToObject(schema, "values");
    if (values == nullptr) {
      cJSON_Delete(schema);
      return nullptr;
    }

    for (const auto& value : allowed_values_) {
      cJSON* value_obj = cJSON_CreateObject();
      if (value_obj == nullptr) {
        cJSON_Delete(schema);
        return nullptr;
      }

      if (cJSON_AddStringToObject(value_obj, "name", value.name) == nullptr ||
          cJSON_AddStringToObject(value_obj, "description", value.description) == nullptr) {
        cJSON_Delete(value_obj);
        cJSON_Delete(schema);
        return nullptr;
      }

      cJSON_AddItemToArray(values, value_obj);
    }

    return schema;
  }

 private:
  std::vector<EnumValue> allowed_values_;
  Getter getter_;
  Setter setter_;
};

/**
 * @brief Boolean parameter with semantic names
 *
 * Example:
 * @code
 *   // Sidetone enabled/disabled
 *   BooleanParameter enabled_param(
 *       "audio.enabled",
 *       "Sidetone enabled",
 *       "on", "off",
 *       [](const DeviceConfig& cfg) { return cfg.audio.sidetone_enabled; },
 *       [](DeviceConfig& cfg, bool val) { cfg.audio.sidetone_enabled = val; }
 *   );
 *
 *   // Late release consider/forget
 *   BooleanParameter late_param(
 *       "keying.late",
 *       "Consider late release",
 *       "consider", "forget",
 *       [](const DeviceConfig& cfg) { return cfg.keying.consider_late_release; },
 *       [](DeviceConfig& cfg, bool val) { cfg.keying.consider_late_release = val; }
 *   );
 * @endcode
 */
class BooleanParameter : public Parameter {
 public:
  using Getter = std::function<bool(const DeviceConfig&)>;
  using Setter = std::function<void(DeviceConfig&, bool)>;

  BooleanParameter(const char* name, const char* description,
                   const char* true_name, const char* false_name,
                   Getter getter, Setter setter)
      : Parameter(name, description, ""),
        true_name_(true_name), false_name_(false_name),
        getter_(std::move(getter)), setter_(std::move(setter)) {}

  const char* GetTypeName() const override { return "bool"; }
  const char* GetTrueName() const { return true_name_; }
  const char* GetFalseName() const { return false_name_; }

  std::string GetRangeDescription() const override {
    std::string result = true_name_;
    result += "/";
    result += false_name_;
    return result;
  }

  bool Validate(const char* value_str, std::string* error) const override {
    if (strcasecmp(value_str, true_name_) == 0 ||
        strcasecmp(value_str, false_name_) == 0) {
      return true;
    }

    if (error) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Value must be '%s' or '%s'", true_name_, false_name_);
      *error = buf;
    }

    return false;
  }

  bool Execute(const char* value_str, DeviceConfig& config,
               std::string* result_message) override {
    bool value;
    if (strcasecmp(value_str, true_name_) == 0) {
      value = true;
    } else if (strcasecmp(value_str, false_name_) == 0) {
      value = false;
    } else {
      std::string error;
      Validate(value_str, &error);
      if (result_message) *result_message = "ERR " + error;
      return false;
    }

    setter_(config, value);

    if (result_message) {
      char buf[128];
      snprintf(buf, sizeof(buf), "OK %s=%s", GetName(), value ? true_name_ : false_name_);
      *result_message = buf;
    }

    return true;
  }

  std::string GetCurrentValue(const DeviceConfig& config) const override {
    return getter_(config) ? true_name_ : false_name_;
  }

  cJSON* CreateJsonSchema() const override {
    cJSON* schema = cJSON_CreateObject();
    if (schema == nullptr) {
      return nullptr;
    }

    if (cJSON_AddStringToObject(schema, "name", GetName()) == nullptr ||
        cJSON_AddStringToObject(schema, "type", "bool") == nullptr ||
        cJSON_AddStringToObject(schema, "widget", "checkbox") == nullptr ||
        cJSON_AddStringToObject(schema, "true", true_name_) == nullptr ||
        cJSON_AddStringToObject(schema, "false", false_name_) == nullptr ||
        cJSON_AddStringToObject(schema, "description", GetDescription()) == nullptr ||
        cJSON_AddStringToObject(schema, "category", GetCategory()) == nullptr) {
      cJSON_Delete(schema);
      return nullptr;
    }

    return schema;
  }

 private:
  const char* true_name_;
  const char* false_name_;
  Getter getter_;
  Setter setter_;
};

/**
 * @brief String parameter with length bounds and optional custom validation.
 *
 * Supports masking for sensitive values (e.g., passwords) so that exported help/status
 * strings do not leak secrets.
 */
class StringParameter : public Parameter {
 public:
  using Getter = std::function<std::string(const DeviceConfig&)>;
  using Setter = std::function<void(DeviceConfig&, std::string_view)>;
  using Validator = std::function<bool(std::string_view, std::string*)>;

  StringParameter(const char* name, const char* description,
                  size_t min_length, size_t max_length,
                  Getter getter, Setter setter,
                  Validator validator = nullptr,
                  const char* mask_token = "")
      : Parameter(name, description, ""),
        min_length_(min_length),
        max_length_(max_length),
        getter_(std::move(getter)),
        setter_(std::move(setter)),
        validator_(std::move(validator)),
        mask_token_(mask_token != nullptr ? mask_token : "") {}

  const char* GetTypeName() const override { return "string"; }

  std::string GetRangeDescription() const override {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lu-%lu chars",
                  static_cast<unsigned long>(min_length_),
                  static_cast<unsigned long>(max_length_));
    return buf;
  }

  bool Validate(const char* value_str, std::string* error) const override {
    if (value_str == nullptr) {
      if (error) {
        *error = "Value cannot be null";
      }
      return false;
    }

    const std::string_view value(value_str);
    const size_t length = value.size();

    if (length < min_length_ || length > max_length_) {
      if (error) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Length must be %lu-%lu characters",
                      static_cast<unsigned long>(min_length_),
                      static_cast<unsigned long>(max_length_));
        *error = buf;
      }
      return false;
    }

    for (char ch : value) {
      const unsigned char uch = static_cast<unsigned char>(ch);
      if (uch < 32 || uch > 126) {
        if (error) {
          *error = "Value must contain printable ASCII characters only";
        }
        return false;
      }
    }

    if (validator_) {
      if (!validator_(value, error)) {
        if (error && error->empty()) {
          *error = "Value failed custom validation";
        }
        return false;
      }
    }

    return true;
  }

  bool Execute(const char* value_str, DeviceConfig& config,
               std::string* result_message) override {
    std::string error;
    if (!Validate(value_str, &error)) {
      if (result_message) {
        *result_message = "ERR " + error;
      }
      return false;
    }

    const std::string_view value(value_str);
    setter_(config, value);

    if (result_message) {
      if (!mask_token_.empty() && !value.empty()) {
        *result_message = "OK " + std::string(GetName()) + "=" + mask_token_;
      } else {
        *result_message = "OK " + std::string(GetName()) + "=" + std::string(value);
      }
    }

    return true;
  }

  std::string GetCurrentValue(const DeviceConfig& config) const override {
    std::string current = getter_(config);
    if (!mask_token_.empty() && !current.empty()) {
      return mask_token_;
    }
    return current;
  }

  cJSON* CreateJsonSchema() const override {
    cJSON* schema = cJSON_CreateObject();
    if (schema == nullptr) {
      return nullptr;
    }

    if (cJSON_AddStringToObject(schema, "name", GetName()) == nullptr ||
        cJSON_AddStringToObject(schema, "type", "string") == nullptr ||
        cJSON_AddStringToObject(schema, "widget", "text_input") == nullptr ||
        cJSON_AddNumberToObject(schema, "min_length", static_cast<double>(min_length_)) == nullptr ||
        cJSON_AddNumberToObject(schema, "max_length", static_cast<double>(max_length_)) == nullptr ||
        cJSON_AddStringToObject(schema, "description", GetDescription()) == nullptr ||
        cJSON_AddStringToObject(schema, "category", GetCategory()) == nullptr) {
      cJSON_Delete(schema);
      return nullptr;
    }

    if (!mask_token_.empty()) {
      if (cJSON_AddTrueToObject(schema, "masked") == nullptr) {
        cJSON_Delete(schema);
        return nullptr;
      }
    }

    return schema;
  }

 private:
  size_t min_length_;
  size_t max_length_;
  Getter getter_;
  Setter setter_;
  Validator validator_;
  std::string mask_token_;
};

}  // namespace config
