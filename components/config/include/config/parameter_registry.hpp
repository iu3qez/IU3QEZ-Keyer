#pragma once

#include <memory>
#include <string>
#include <vector>
#include "config/parameter_metadata.hpp"

namespace config {

/**
 * @brief Centralized registry for all configuration parameters
 *
 * The ParameterRegistry manages a collection of Parameter objects, providing:
 * - Parameter registration (takes ownership via unique_ptr)
 * - Lookup by name (subsystem.param format)
 * - Filtering by subsystem prefix and visibility
 * - Auto-generation of help text
 * - JSON schema export for Web UI
 *
 * Global singleton access via GetGlobalRegistry().
 *
 * Example usage:
 * @code
 *   // Register parameters (typically done once at boot)
 *   auto& registry = GetGlobalRegistry();
 *   registry.Register(std::make_unique<IntParameter<100, 2000>>(
 *       "audio.freq", "Sidetone frequency", "Hz", getter, setter));
 *
 *   // Find and execute
 *   Parameter* param = registry.Find("audio.freq");
 *   if (param) {
 *     std::string result;
 *     param->Execute("700", device_config, &result);
 *   }
 *
 *   // Generate help text for subsystem
 *   std::string help = registry.GenerateHelpText("audio", device_config);
 * @endcode
 */
class ParameterRegistry {
 public:
  ParameterRegistry() = default;
  ~ParameterRegistry() = default;

  // Non-copyable (singleton pattern)
  ParameterRegistry(const ParameterRegistry&) = delete;
  ParameterRegistry& operator=(const ParameterRegistry&) = delete;

  /**
   * @brief Register a parameter (takes ownership)
   *
   * @param param Parameter to register (must be unique_ptr)
   *
   * Example:
   * @code
   *   registry.Register(std::make_unique<IntParameter<0, 100>>(
   *       "audio.volume", "Sidetone volume", "%", getter, setter));
   * @endcode
   */
  void Register(std::unique_ptr<Parameter> param);

  /**
   * @brief Find parameter by name
   *
   * @param name Parameter name in "subsystem.param" format (e.g., "audio.freq")
   * @return Pointer to parameter if found, nullptr otherwise
   *
   * Example:
   * @code
   *   Parameter* param = registry.Find("keying.wpm");
   *   if (param && param->IsVisible(config)) {
   *     std::string value = param->GetCurrentValue(config);
   *   }
   * @endcode
   */
  Parameter* Find(const char* name) const;

  /**
   * @brief Get all visible parameters for a subsystem
   *
   * @param subsystem_prefix Subsystem name (e.g., "audio", "keying")
   * @param config Current configuration (for visibility checks)
   * @return Vector of visible parameter pointers
   *
   * Example:
   * @code
   *   auto audio_params = registry.GetVisibleParameters("audio", config);
   *   for (Parameter* p : audio_params) {
   *     printf("%s = %s\n", p->GetName(), p->GetCurrentValue(config).c_str());
   *   }
   * @endcode
   */
  std::vector<Parameter*> GetVisibleParameters(const char* subsystem_prefix,
                                                const DeviceConfig& config) const;

  /**
   * @brief Export all parameters as JSON schema with widget hints for Web UI
   *
   * @param config Current configuration (for visibility checks)
   * @return JSON document with all visible parameters including type, widget, ranges, metadata
   *
   * Output format (Task 5.7.7):
   * @code
   *   {
   *     "parameters": [
   *       {
   *         "name": "audio.freq",
   *         "type": "int",
   *         "widget": "number_input",
   *         "min": 100,
   *         "max": 2000,
   *         "unit": "Hz",
   *         "description": "Sidetone frequency"
   *       },
   *       {
   *         "name": "keying.preset",
   *         "type": "enum",
   *         "widget": "dropdown",
   *         "values": [
   *           {"name": "V0", "description": "SuperKeyer with dot+dash memory"},
   *           {"name": "V1", "description": "SuperKeyer with dot-only memory"},
   *           ...
   *         ],
   *         "description": "Iambic keying preset"
   *       },
   *       {
   *         "name": "keying.window_open",
   *         "type": "float",
   *         "widget": "slider",
   *         "min": 0.0,
   *         "max": 100.0,
   *         "precision": 1,
   *         "unit": "%",
   *         "description": "Memory window open threshold"
   *       },
   *       {
   *         "name": "audio.enabled",
   *         "type": "bool",
   *         "widget": "checkbox",
   *         "true": "on",
   *         "false": "off",
   *         "description": "Sidetone enabled"
   *       }
   *     ]
   *   }
   * @endcode
   *
   * WIDGET HINTS (Task 5.7.7.2):
   * - "number_input": IntParameter - spin box or numeric text field
   * - "slider": FloatParameter - range slider for continuous values
   * - "dropdown": EnumParameter - select menu with predefined options
   * - "checkbox": BooleanParameter - toggle switch or checkbox
   *
   * USAGE FOR HTTP ENDPOINTS (Task 5.7.7.4):
   * Generate JSON schema on-demand for Web UI configuration page:
   * @code
   *   // In HTTP handler for GET /api/config/schema
   *   std::string json_schema = param_registry.ExportJsonSchema(device_config);
   *   httpd_resp_set_type(req, "application/json");
   *   httpd_resp_sendstr(req, json_schema.c_str());
   * @endcode
   *
   * CACHING STRATEGY (optional optimization):
   * @code
   *   // Cache JSON schema at boot or config change to avoid runtime generation cost
   *   static std::string cached_schema;
   *   static KeyingPreset last_preset = KeyingPreset::kManual;
   *
   *   // Regenerate only if visibility might have changed (e.g., preset changed)
   *   if (device_config.keying.preset != last_preset) {
   *     cached_schema = param_registry.ExportJsonSchema(device_config);
   *     last_preset = device_config.keying.preset;
   *   }
   *   return cached_schema;
   * @endcode
   *
   * NOTES:
   * - Only visible parameters are exported (see IsVisible() in Task 5.7.6)
   * - Web UI uses widget hints to auto-generate appropriate form controls
   * - Min/max/unit metadata enables client-side validation and proper formatting
   * - Enum descriptions help users understand preset differences without documentation
   */
  std::string ExportJsonSchema(const DeviceConfig& config) const;

  /**
   * @brief Generate help text for a subsystem
   *
   * @param subsystem_prefix Subsystem name (e.g., "audio", "keying")
   * @param config Current configuration (for visibility checks)
   * @return Human-readable help text with parameter names, types, ranges, current values
   *
   * Output format:
   * @code
   *   Audio Parameters:
   *     freq (int): Sidetone frequency [100-2000 Hz] (current: 700)
   *     volume (int): Sidetone volume [0-100 %] (current: 50)
   *     enabled (bool): Sidetone enabled [on/off] (current: off)
   * @endcode
   */
  std::string GenerateHelpText(const char* subsystem_prefix,
                                const DeviceConfig& config) const;

  /**
   * @brief Get number of registered parameters
   *
   * @return Count of parameters in registry
   */
  size_t GetParameterCount() const { return parameters_.size(); }

 private:
  std::vector<std::unique_ptr<Parameter>> parameters_;
};

/**
 * @brief Register all device parameters into a registry
 *
 * Populates the provided registry with all device parameters:
 * - General: callsign
 * - Audio: freq, volume, fade_in, fade_out, enabled
 * - Keying: preset, wpm, window, late, dit_memory, dah_memory, latch
 *
 * This function should be called from ApplicationController::Initialize()
 * after loading configuration but before console initialization.
 *
 * @param registry ParameterRegistry to populate (passed by reference)
 *
 * Example:
 * @code
 *   class ApplicationController {
 *     config::ParameterRegistry param_registry_;
 *
 *     void Initialize() {
 *       config_storage_->Initialize();
 *       device_config_ = config_storage_->LoadOrDefault();
 *
 *       config::RegisterAllParameters(param_registry_);  // <-- Populate registry
 *
 *       console_handler_->SetParameterRegistry(&param_registry_);  // <-- Inject
 *       console_->Initialize();
 *     }
 *   };
 * @endcode
 */
void RegisterAllParameters(ParameterRegistry& registry);

}  // namespace config
