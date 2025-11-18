/**
 * @file console_parameter_bridge.cpp
 * @brief Bridge between serial console and YAML parameter system
 *
 * Provides helper functions to interface with config::ParameterRegistry:
 * - GetParameterList(): Query visible parameters by subsystem prefix
 * - GetParameterValue(): Read current parameter value
 * - SetParameterValue(): Write parameter value with validation
 * - HandleShowCommand(): "show" command implementation
 * - HandleSetCommand(): "set" command implementation
 *
 * Thread safety: Uses existing mutex/locks from config subsystem.
 * Parameter changes via console are validated against min/max/enum constraints.
 */

#include "ui/console_parameter_bridge.hpp"
#include "ui/serial_console.hpp"
#include "esp_log.h"
#include "esp_system.h"      // For esp_restart()
#include "nvs_flash.h"       // For nvs_flash_erase()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   // For vTaskDelay()
#include <cstring>
#include <algorithm>

namespace ui {

static const char* TAG = "console_param_bridge";

// Forward declaration for console output (global instance set in SerialConsolePhase)
extern ui::SerialConsole* g_console_instance;

//=============================================================================
// Helper Functions
//=============================================================================

std::vector<config::Parameter*> GetParameterList(config::ParameterRegistry* registry,
                                                  const std::string& prefix,
                                                  const config::DeviceConfig& config) {
    if (!registry) {
        return {};
    }

    // If prefix is empty, get all visible parameters by trying common prefixes
    if (prefix.empty()) {
        std::vector<config::Parameter*> all_params;
        const char* subsystems[] = {"general", "keying", "audio", "wifi", "paddle", "tx", "remote", "server"};

        for (const char* subsystem : subsystems) {
            auto params = registry->GetVisibleParameters(subsystem, config);
            all_params.insert(all_params.end(), params.begin(), params.end());
        }

        return all_params;
    }

    // Get visible parameters for specified prefix
    return registry->GetVisibleParameters(prefix.c_str(), config);
}

bool GetParameterValue(config::ParameterRegistry* registry,
                       const std::string& name,
                       const config::DeviceConfig& config,
                       std::string* out_value,
                       std::string* error_msg) {
    if (!registry) {
        if (error_msg) *error_msg = "Parameter registry not available";
        return false;
    }

    // Find parameter by name
    config::Parameter* param = registry->Find(name.c_str());
    if (!param) {
        if (error_msg) *error_msg = "Parameter '" + name + "' not found";
        return false;
    }

    // Check visibility
    if (!param->IsVisible(config)) {
        if (error_msg) *error_msg = "Parameter '" + name + "' is not visible in current mode";
        return false;
    }

    // Get current value
    if (out_value) {
        *out_value = param->GetCurrentValue(config);
    }

    return true;
}

bool SetParameterValue(config::ParameterRegistry* registry,
                       const std::string& name,
                       const std::string& value_str,
                       config::DeviceConfig& config,
                       std::string* error_msg) {
    if (!registry) {
        if (error_msg) *error_msg = "Parameter registry not available";
        return false;
    }

    // Find parameter by name
    config::Parameter* param = registry->Find(name.c_str());
    if (!param) {
        // Debug: log registry stats when parameter not found
        char debug_msg[256];
        snprintf(debug_msg, sizeof(debug_msg),
                "Parameter '%s' not found (registry has %zu parameters)",
                name.c_str(), registry->GetParameterCount());
        if (error_msg) *error_msg = debug_msg;
        return false;
    }

    // Check visibility
    if (!param->IsVisible(config)) {
        if (error_msg) *error_msg = "Parameter '" + name + "' is not visible in current mode";
        return false;
    }

    // Execute parameter update (includes validation)
    std::string result_msg;
    bool success = param->Execute(value_str.c_str(), config, &result_msg);

    if (!success) {
        // Parse error message from result (typically starts with "ERR")
        if (error_msg) {
            if (result_msg.find("ERR") == 0) {
                *error_msg = result_msg.substr(4);  // Skip "ERR "
            } else {
                *error_msg = result_msg;
            }
        }
        return false;
    }

    return true;
}

//=============================================================================
// Command Handlers
//=============================================================================

int HandleShowCommand(const std::vector<std::string>& args,
                      config::ParameterRegistry* registry,
                      config::DeviceConfig* config) {
    if (!registry || !config || !g_console_instance) {
        ESP_LOGE(TAG, "show: Invalid arguments (registry=%p, config=%p, console=%p)",
                 registry, config, g_console_instance);
        return -1;
    }

    // Case 1: "show" (no args) → List all parameters
    if (args.size() == 1) {
        auto params = GetParameterList(registry, "", *config);

        if (params.empty()) {
            g_console_instance->Print("No parameters available\r\n");
            return 0;
        }

        g_console_instance->Printf("Available parameters (%d):\r\n", (int)params.size());
        for (auto* param : params) {
            std::string value = param->GetCurrentValue(*config);
            const char* unit = param->GetUnit();

            if (unit && strlen(unit) > 0) {
                g_console_instance->Printf("  %s = %s (%s)\r\n",
                                          param->GetName(), value.c_str(), unit);
            } else {
                g_console_instance->Printf("  %s = %s\r\n",
                                          param->GetName(), value.c_str());
            }
        }
        return 0;
    }

    // Case 2: "show <parameter|prefix>"
    std::string target = args[1];

    // Special case: "show debug" → Display current debug settings
    if (target == "debug") {
        g_console_instance->Print("Debug settings:\r\n");
        g_console_instance->Print("  Global log level: (transient, changes not shown)\r\n");
        g_console_instance->Print("  Per-tag levels: (transient, changes not shown)\r\n");
        g_console_instance->Print("\r\n");
        g_console_instance->Print("Note: Debug settings are not persisted to NVS\r\n");
        g_console_instance->Print("Use 'debug level <level>' to set global log level\r\n");
        g_console_instance->Print("Use 'debug show <tag>' to enable verbose logging for a tag\r\n");
        g_console_instance->Print("Type 'debug' for full usage\r\n");
        return 0;
    }

    // Special case: "show *" or "show *.*" means show all parameters
    if (target == "*" || target == "*.*") {
        target = "";  // Empty string = all parameters
    }

    // Check if it's a wildcard pattern (ends with .* or just subsystem name)
    bool is_prefix = (target.find('*') != std::string::npos) ||
                     (target.find('.') == std::string::npos);

    if (is_prefix) {
        // Remove trailing .* if present
        if (target.length() >= 2 && target.substr(target.length() - 2) == ".*") {
            target = target.substr(0, target.length() - 2);
        }

        // List all parameters with this prefix (empty string = all)
        auto params = GetParameterList(registry, target, *config);

        if (params.empty()) {
            g_console_instance->Printf("No parameters found with prefix '%s'\r\n", target.c_str());
            return -1;
        }

        g_console_instance->Printf("Parameters matching '%s.*' (%d):\r\n", target.c_str(), (int)params.size());
        for (auto* param : params) {
            std::string value = param->GetCurrentValue(*config);
            const char* unit = param->GetUnit();

            if (unit && strlen(unit) > 0) {
                g_console_instance->Printf("  %s = %s (%s)\r\n",
                                          param->GetName(), value.c_str(), unit);
            } else {
                g_console_instance->Printf("  %s = %s\r\n",
                                          param->GetName(), value.c_str());
            }
        }
        return 0;
    }

    // Case 3: "show <full_parameter_name>"
    std::string value;
    std::string error_msg;

    if (!GetParameterValue(registry, target, *config, &value, &error_msg)) {
        g_console_instance->Printf("Error: %s\r\n", error_msg.c_str());
        return -1;
    }

    // Find parameter to get unit
    config::Parameter* param = registry->Find(target.c_str());
    if (param) {
        const char* unit = param->GetUnit();
        if (unit && strlen(unit) > 0) {
            g_console_instance->Printf("%s = %s (%s)\r\n", target.c_str(), value.c_str(), unit);
        } else {
            g_console_instance->Printf("%s = %s\r\n", target.c_str(), value.c_str());
        }
    }

    return 0;
}

int HandleSetCommand(const std::vector<std::string>& args,
                     config::ParameterRegistry* registry,
                     config::DeviceConfig* config) {
    if (!registry || !config || !g_console_instance) {
        ESP_LOGE(TAG, "set: Invalid arguments (registry=%p, config=%p, console=%p)",
                 registry, config, g_console_instance);
        return -1;
    }

    // If no arguments, show list of available parameters
    if (args.size() == 1) {
        g_console_instance->Print("Available parameters:\r\n\r\n");

        auto params = GetParameterList(registry, "", *config);
        if (params.empty()) {
            g_console_instance->Print("No parameters available\r\n");
            return -1;
        }

        // Group by subsystem for better readability
        std::string last_subsystem;
        for (auto* param : params) {
            std::string full_name = param->GetName();

            // Extract subsystem (part before first '.')
            size_t dot_pos = full_name.find('.');
            std::string subsystem = (dot_pos != std::string::npos)
                ? full_name.substr(0, dot_pos)
                : full_name;

            // Print subsystem header if changed
            if (subsystem != last_subsystem) {
                if (!last_subsystem.empty()) {
                    g_console_instance->Print("\r\n");
                }
                g_console_instance->Printf("[%s]\r\n", subsystem.c_str());
                last_subsystem = subsystem;
            }

            // Print parameter with description
            g_console_instance->Printf("  %-25s - %s\r\n",
                                      full_name.c_str(),
                                      param->GetDescription());
        }

        g_console_instance->Print("\r\nUsage: set <parameter> <value>\r\n");
        g_console_instance->Print("Example: set keying.wpm 25\r\n");
        return 0;
    }

    // Syntax: set <parameter> <value>
    if (args.size() < 3) {
        g_console_instance->Print("Usage: set <parameter> <value>\r\n");
        g_console_instance->Print("Type 'set' without arguments to see available parameters\r\n");
        return -1;
    }

    std::string param_name = args[1];
    std::string value_str = args[2];

    // Attempt to set parameter value
    std::string error_msg;
    if (!SetParameterValue(registry, param_name, value_str, *config, &error_msg)) {
        g_console_instance->Printf("Error: %s\r\n", error_msg.c_str());

        // Try to get parameter to show constraints
        config::Parameter* param = registry->Find(param_name.c_str());
        if (param) {
            std::string range = param->GetRangeDescription();
            if (!range.empty()) {
                g_console_instance->Printf("Valid range: %s\r\n", range.c_str());
            }
        }

        return -1;
    }

    // Success - show new value
    std::string new_value;
    if (GetParameterValue(registry, param_name, *config, &new_value, nullptr)) {
        g_console_instance->Printf("%s = %s\r\n", param_name.c_str(), new_value.c_str());
        g_console_instance->Print("Note: Use 'save' command to persist changes to NVS\r\n");
    }

    return 0;
}

int HandleSaveCommand(const std::vector<std::string>& args,
                      config::Storage* storage,
                      config::DeviceConfig* config) {
    // Validate arguments
    if (!storage || !config || !g_console_instance) {
        ESP_LOGE("console_param", "HandleSaveCommand: null pointer (storage=%p, config=%p, console=%p)",
                 storage, config, g_console_instance);
        return -1;
    }

    // No arguments needed for save
    if (args.size() > 1) {
        g_console_instance->Print("Usage: save\r\n");
        g_console_instance->Print("Saves current configuration to NVS flash\r\n");
        return -1;
    }

    g_console_instance->Print("Saving configuration to NVS...\r\n");

    // Save configuration to NVS (with automatic backup)
    esp_err_t err = storage->Save(*config, true);

    if (err == ESP_OK) {
        g_console_instance->Print("Configuration saved successfully!\r\n");
        return 0;
    } else {
        g_console_instance->Printf("Error saving configuration: %s (0x%x)\r\n",
                                   esp_err_to_name(err), err);
        return -1;
    }
}

void RegisterParameterCommands(SerialConsole* console,
                                config::ParameterRegistry* registry,
                                config::DeviceConfig* config,
                                config::Storage* storage) {
    if (!console) {
        ESP_LOGE(TAG, "RegisterParameterCommands: console is null");
        return;
    }

    // Register 'show' command
    console->RegisterCommand("show",
        [registry, config](const std::vector<std::string>& args) -> int {
            return HandleShowCommand(args, registry, config);
        },
        "show [*|parameter|prefix.*] - Display parameter values (use * for all)");

    // Register 'set' command
    console->RegisterCommand("set",
        [registry, config](const std::vector<std::string>& args) -> int {
            return HandleSetCommand(args, registry, config);
        },
        "set <parameter> <value> - Set parameter value (type 'set' for list)");

    // Register 'save' command
    console->RegisterCommand("save",
        [storage, config](const std::vector<std::string>& args) -> int {
            return HandleSaveCommand(args, storage, config);
        },
        "save - Save configuration to NVS flash");

    ESP_LOGI(TAG, "Registered parameter commands: show, set, save");
}

} // namespace ui
