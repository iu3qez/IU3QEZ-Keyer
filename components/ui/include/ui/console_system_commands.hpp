/**
 * @file console_system_commands.hpp
 * @brief System commands for serial console (debug, reboot, factory-reset)
 *
 * Provides system-level commands:
 * - debug: Change ESP-IDF log level at runtime
 * - reboot: Restart the device
 * - factory-reset: Erase NVS and restore factory defaults
 */

#pragma once

#include <string>
#include <vector>
#include "config/device_config.hpp"

namespace remote {
class RemoteCwClient;
class RemoteCwServer;
}

namespace keying_subsystem {
class KeyingSubsystem;
}

namespace morse_decoder {
class MorseDecoder;
}

namespace ui {

// Forward declaration for SerialConsole
class SerialConsole;

/**
 * @brief Handle "reboot" command
 *
 * Syntax: `reboot [bootloader]`
 *
 * Restarts the ESP32-S3 immediately, or enters UF2 bootloader mode if 'bootloader' argument is provided.
 *
 * Options:
 * - No argument: Normal reboot
 * - bootloader: Enter UF2 bootloader for USB firmware updates
 *
 * @param args Command arguments (args[0] is "reboot", args[1] is optional "bootloader")
 * @return 0 on success (never returns - device reboots)
 */
int HandleRebootCommand(const std::vector<std::string>& args);

/**
 * @brief Handle "debug" command
 *
 * Syntax: `debug [level]`
 *
 * Sets ESP-IDF log level for all tags.
 * Levels: none, error, warn, info, debug, verbose
 *
 * @param args Command arguments (args[0] is "debug", args[1] is optional level)
 * @return 0 on success, -1 on error
 */
int HandleDebugCommand(const std::vector<std::string>& args);

/**
 * @brief Tab completion handler for "debug" command
 *
 * Provides autocomplete for:
 * - Log levels: none, error, warn, info, debug, verbose
 * - Special commands: tags, timeline
 * - All known logging tags (auto-generated from codebase)
 * - Tag-specific log level setting
 *
 * @param input Current input text
 * @param cursor_pos Cursor position in input
 * @return Vector of possible completions
 */
std::vector<std::string> TabCompleteDebug(const std::string& input, size_t cursor_pos);

/**
 * @brief Tab completion handler for "reboot" command
 *
 * Provides autocomplete for:
 * - bootloader: Enter UF2 bootloader mode for USB firmware updates
 *
 * @param input Current input text
 * @param cursor_pos Cursor position in input
 * @return Vector of possible completions
 */
std::vector<std::string> TabCompleteReboot(const std::string& input, size_t cursor_pos);

/**
 * @brief Handle "factory-reset" command
 *
 * Syntax: `factory-reset confirm`
 *
 * Erases NVS flash and reboots to factory defaults.
 * Requires "confirm" argument to prevent accidental resets.
 *
 * @param args Command arguments (args[0] is "factory-reset", args[1] must be "confirm")
 * @return 0 on success (never returns - device reboots after reset)
 */
int HandleFactoryResetCommand(const std::vector<std::string>& args);

/**
 * @brief Handle "remote" command
 *
 * Syntax: `remote [start|stop|status]`
 *
 * Controls remote CW client connection:
 * - start: Initiate connection to remote server
 * - stop: Close connection and stop client
 * - status: Display connection state and statistics
 *
 * @param args Command arguments (args[0] is "remote", args[1] is subcommand)
 * @return 0 on success, -1 on error
 */
int HandleRemoteCommand(const std::vector<std::string>& args);

/**
 * @brief Handle "server" command
 *
 * Syntax: `server [start|stop|status]`
 *
 * Controls remote CW server:
 * - start: Start listening for client connections
 * - stop: Stop server and close connections
 * - status: Display server state and client info
 *
 * @param args Command arguments (args[0] is "server", args[1] is subcommand)
 * @return 0 on success, -1 on error
 */
int HandleServerCommand(const std::vector<std::string>& args);

/**
 * @brief Set remote client instance for console commands
 *
 * Must be called before remote commands can be used.
 * Typically called by RemoteClientPhase during initialization.
 *
 * @param client Pointer to RemoteCwClient instance (non-owning)
 */
void SetRemoteClient(remote::RemoteCwClient* client);

/**
 * @brief Set remote server instance for console commands
 *
 * Must be called before server commands can be used.
 * Typically called by RemoteServerPhase during initialization.
 *
 * @param server Pointer to RemoteCwServer instance (non-owning)
 */
void SetRemoteServer(remote::RemoteCwServer* server);

/**
 * @brief Set keying subsystem instance for console debug commands
 *
 * Must be called before keying-debug command can be used.
 * Typically called by KeyingSubsystemPhase during initialization.
 *
 * @param keying Pointer to KeyingSubsystem instance (non-owning)
 */
void SetKeyingSubsystem(keying_subsystem::KeyingSubsystem* keying);

/**
 * @brief Handle "keying-debug" command
 *
 * Syntax: `keying-debug`
 *
 * Dumps complete paddle engine state for troubleshooting lockups.
 * Shows state machine state, memory flags, latch flags, paddle states, etc.
 *
 * @param args Command arguments (args[0] is "keying-debug")
 * @return 0 on success, -1 on error
 */
int HandleKeyingDebugCommand(const std::vector<std::string>& args);

/**
 * @brief Handle "debug timeline" command
 *
 * Syntax: `debug timeline`
 *
 * Dumps timeline hooks status for troubleshooting LOGIC overlay events.
 * Shows TimelineEmitter connection status and hook function pointers.
 *
 * @param args Command arguments (args[0] is "debug", args[1] is "timeline")
 * @return 0 on success, -1 on error
 */
int HandleTimelineDebugCommand(const std::vector<std::string>& args);

/**
 * @brief Set morse decoder instance for console commands
 *
 * Must be called before decoder command can be used.
 * Typically called by initialization pipeline after MorseDecoder creation.
 *
 * @param decoder Pointer to MorseDecoder instance (non-owning)
 */
void SetMorseDecoder(morse_decoder::MorseDecoder* decoder);

/**
 * @brief Handle "decoder" command
 *
 * Syntax: `decoder [start|stop|reset|stats|text]`
 *
 * Controls morse code decoder:
 * - start: Enable decoder (keying events → text)
 * - stop: Disable decoder (no processing)
 * - reset: Clear decoded text buffer and reset timing
 * - stats: Show timing stats (WPM, dit/dah averages, ratio, samples)
 * - text: Display decoded text buffer (last 100 chars)
 *
 * @param args Command arguments (args[0] is "decoder", args[1] is subcommand)
 * @return 0 on success, -1 on error
 */
int HandleDecoderCommand(const std::vector<std::string>& args);

/**
 * @brief Handle "cpu" command
 *
 * Syntax: `cpu`
 *
 * Displays CPU usage statistics per task with runtime and percentage.
 * Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y.
 *
 * @param args Command arguments (args[0] is "cpu")
 * @return 0 on success, -1 on error
 */
int HandleCpuCommand(const std::vector<std::string>& args);

/**
 * @brief Handle "tasks" command
 *
 * Syntax: `tasks`
 *
 * Displays task list with state, priority, and stack high water mark.
 * Requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y.
 *
 * @param args Command arguments (args[0] is "tasks")
 * @return 0 on success, -1 on error
 */
int HandleTasksCommand(const std::vector<std::string>& args);

/**
 * @brief Handle "system" command
 *
 * Syntax: `system`
 *
 * Displays complete system statistics in JSON format:
 * - Uptime (seconds, minutes, hours)
 * - Heap memory (free, minimum, fragmentation)
 * - CPU usage per task
 * - Task states and priorities
 *
 * @param args Command arguments (args[0] is "system")
 * @return 0 on success, -1 on error
 */
int HandleSystemCommand(const std::vector<std::string>& args);

/**
 * @brief Register all system commands
 *
 * Registers: reboot, debug, factory-reset, remote, server, keying-debug, decoder, cpu, tasks, system
 * Call this before SerialConsole::Init().
 *
 * @param console SerialConsole instance to register commands with
 */
void RegisterSystemCommands(SerialConsole* console);

}  // namespace ui
