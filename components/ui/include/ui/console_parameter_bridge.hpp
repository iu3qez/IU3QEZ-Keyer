/**
 * @file console_parameter_bridge.hpp
 * @brief Bridge between serial console and YAML parameter system
 *
 * Provides helper functions to interface with config::ParameterRegistry:
 * - GetParameterList(): Query visible parameters by subsystem prefix
 * - GetParameterValue(): Read current parameter value
 * - SetParameterValue(): Write parameter value with validation
 * - HandleShowCommand(): Implementation for "show" command
 * - HandleSetCommand(): Implementation for "set" command
 *
 * Thread safety: Uses existing mutex/locks from config subsystem.
 * Parameter changes via console are validated against min/max/enum constraints.
 */

#pragma once

#include <string>
#include <vector>
#include "config/parameter_registry.hpp"
#include "config/device_config.hpp"

namespace ui {

/**
 * @brief Get list of visible parameters matching prefix
 *
 * @param registry ParameterRegistry to query
 * @param prefix Subsystem prefix (e.g., "keying", "audio") or empty for all
 * @param config Current device configuration (for visibility checks)
 * @return Vector of pointers to visible parameters
 *
 * Example:
 * @code
 *   auto params = GetParameterList(registry, "keying", config);
 *   for (auto* p : params) {
 *     printf("%s = %s\n", p->GetName(), p->GetCurrentValue(config).c_str());
 *   }
 * @endcode
 */
std::vector<config::Parameter*> GetParameterList(config::ParameterRegistry* registry,
                                                  const std::string& prefix,
                                                  const config::DeviceConfig& config);

/**
 * @brief Get current value of a parameter
 *
 * @param registry ParameterRegistry to query
 * @param name Full parameter name (e.g., "keying.wpm")
 * @param config Current device configuration
 * @param out_value Output parameter value (only set on success)
 * @param error_msg Output error message (only set on failure)
 * @return true on success, false if parameter not found or not visible
 */
bool GetParameterValue(config::ParameterRegistry* registry,
                       const std::string& name,
                       const config::DeviceConfig& config,
                       std::string* out_value,
                       std::string* error_msg);

/**
 * @brief Set parameter value with validation
 *
 * @param registry ParameterRegistry to modify
 * @param name Full parameter name (e.g., "keying.wpm")
 * @param value_str Value string to parse (e.g., "25", "V3", "on")
 * @param config Device configuration to update (modified in-place on success)
 * @param error_msg Output error message (set on validation failure)
 * @return true on success, false on validation failure or parameter not found
 *
 * NOTE: Changes are in-memory only. Use "save" command to persist to NVS.
 */
bool SetParameterValue(config::ParameterRegistry* registry,
                       const std::string& name,
                       const std::string& value_str,
                       config::DeviceConfig& config,
                       std::string* error_msg);

/**
 * @brief Handle "show" command
 *
 * Syntax:
 * - `show` → List all visible parameters with values
 * - `show <parameter>` → Show single parameter value
 * - `show <prefix>.*` → List all parameters matching prefix
 * - `show <prefix>` → Alias for `show <prefix>.*`
 *
 * @param args Command arguments (args[0] is "show")
 * @param registry ParameterRegistry to query
 * @param config Current device configuration
 * @return 0 on success, -1 on error
 */
int HandleShowCommand(const std::vector<std::string>& args,
                      config::ParameterRegistry* registry,
                      config::DeviceConfig* config);

/**
 * @brief Handle "set" command
 *
 * Syntax: `set <parameter> <value>`
 *
 * Examples:
 * - `set keying.wpm 25`
 * - `set audio.sidetone_frequency 700`
 * - `set keying.preset V3`
 *
 * @param args Command arguments (args[0] is "set", args[1] is parameter, args[2] is value)
 * @param registry ParameterRegistry to modify
 * @param config Device configuration to update
 * @return 0 on success, -1 on error
 */
int HandleSetCommand(const std::vector<std::string>& args,
                     config::ParameterRegistry* registry,
                     config::DeviceConfig* config);

/**
 * @brief Handle "save" command
 *
 * Syntax: `save`
 *
 * Persists current in-memory configuration to NVS flash.
 * Creates automatic backup before saving.
 *
 * @param args Command arguments (args[0] is "save")
 * @param storage Storage instance to use for saving
 * @param config Device configuration to save
 * @return 0 on success, -1 on error
 */
int HandleSaveCommand(const std::vector<std::string>& args,
                      config::Storage* storage,
                      config::DeviceConfig* config);

// Forward declaration for SerialConsole
class SerialConsole;

/**
 * @brief Register all parameter management commands
 *
 * Registers: show, set, save
 * Call this before SerialConsole::Init() to avoid race conditions.
 *
 * @param console SerialConsole instance to register commands with
 * @param registry ParameterRegistry for parameter access
 * @param config DeviceConfig for parameter values
 * @param storage Storage for save command
 */
void RegisterParameterCommands(SerialConsole* console,
                                config::ParameterRegistry* registry,
                                config::DeviceConfig* config,
                                config::Storage* storage);

}  // namespace ui
