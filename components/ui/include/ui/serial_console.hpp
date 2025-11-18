/**
 * @file serial_console.hpp
 * @brief Serial Console Interface for USB-CDC runtime parameter configuration
 *
 * This console provides an interactive command-line interface over USB-CDC (COM7)
 * for runtime configuration of YAML parameters, debug control, and system commands.
 *
 * Features:
 * - Command history (8 entries)
 * - TAB completion for commands and parameters
 * - Line editing with arrow keys and backspace
 * - Integration with YAML parameter registry
 * - Non-ANSI plain text output for broad compatibility
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <memory>

// Forward declaration for DeviceConfig (MUST be outside ui namespace)
namespace config {
struct DeviceConfig;
}

namespace ui {

// Forward declaration
class SerialConsole;

// Global console instance pointer (used by command handlers in console_parameter_bridge.cpp)
extern SerialConsole* g_console_instance;

/**
 * Maximum line length for console input buffer.
 * Value: 128 bytes - sufficient for typical commands with parameters
 * (e.g., "set keying.wpm 25", "debug level verbose")
 */
constexpr size_t MAX_LINE = 128;

/**
 * Number of commands stored in command history.
 * Value: 8 entries - provides reasonable history depth
 * Memory usage: 8 × 128 bytes = 1KB total history buffer
 */
constexpr size_t HISTORY_SIZE = 8;

/**
 * Maximum number of arguments per command.
 * Value: 8 - supports commands like "set subsystem.param value" plus additional options
 */
constexpr size_t MAX_ARGS = 8;

/**
 * Command handler function signature.
 * @param args Vector of command arguments (args[0] is the command verb)
 * @return int Exit code (0 = success, non-zero = error)
 */
using CommandHandler = std::function<int(const std::vector<std::string>&)>;

/**
 * Tab completion handler function signature.
 * @param partial_input Current input text (e.g., "debug ta")
 * @param cursor_pos Position of cursor in input
 * @return Vector of possible completions (empty if none)
 */
using TabCompleteHandler = std::function<std::vector<std::string>(const std::string&, size_t)>;

/**
 * Command registration structure.
 * Associates a command verb with its handler and help text.
 */
struct Command {
    std::string verb;                      ///< Command name (e.g., "help", "set", "show")
    CommandHandler handler;                ///< Function to execute for this command
    std::string help;                      ///< Help text displayed by "help <command>"
    TabCompleteHandler tab_complete;       ///< Optional tab completion handler for arguments

    Command(const std::string& v, CommandHandler h, const std::string& desc, TabCompleteHandler tc = nullptr)
        : verb(v), handler(h), help(desc), tab_complete(tc) {}
};

/**
 * Serial Console Manager.
 *
 * Manages the USB-CDC console interface with modular architecture:
 * - InputHandler: Processes keyboard input, line editing, history navigation
 * - CommandDispatcher: Routes commands to registered handlers
 * - OutputBuffer: Manages USB-CDC output with buffering
 *
 * The console runs in a dedicated FreeRTOS task with 4KB stack.
 * Thread safety: Parameter access uses existing mutex/locks from config subsystem.
 */
class SerialConsole {
public:
    explicit SerialConsole(config::DeviceConfig* config);
    ~SerialConsole();

    /**
     * Initialize console and create FreeRTOS task.
     * Sets up USB-CDC interface (TINYUSB_CDC_ACM_1) and displays initial prompt.
     */
    void Init();

    /**
     * Register a command handler.
     * @param verb Command name (case-sensitive)
     * @param handler Function to call when command is executed
     * @param help Help text for this command
     * @param tab_complete Optional tab completion handler for arguments
     */
    void RegisterCommand(const std::string& verb, CommandHandler handler, const std::string& help, TabCompleteHandler tab_complete = nullptr);

    /**
     * Main console task loop (called by FreeRTOS task).
     * Processes input, handles line editing, dispatches commands.
     */
    void Task();

    /**
     * Print string to console output.
     * @param str String to output (automatically buffered)
     */
    void Print(const std::string& str);

    /**
     * Printf-style formatted output to console.
     * @param fmt Printf format string
     * @param ... Variable arguments
     */
    void Printf(const char* fmt, ...);

    /**
     * Get list of registered commands (for TAB completion).
     * @return Vector of registered commands
     */
    std::vector<Command>& GetCommands();

private:
    // Forward declarations for modular components
    class InputHandler;         ///< Handles keyboard input and line editing
    class CommandDispatcher;    ///< Routes commands to handlers
    class OutputBuffer;         ///< Manages USB-CDC output buffering

    std::unique_ptr<InputHandler> input_;
    std::unique_ptr<CommandDispatcher> dispatcher_;
    std::unique_ptr<OutputBuffer> output_;
    config::DeviceConfig* config_;  ///< Device configuration for dynamic prompt

    /**
     * Process a completed command line.
     * Tokenizes input and dispatches to CommandDispatcher.
     * @param line Complete command line from user
     */
    void ProcessLine(const std::string& line);

    /**
     * Get current prompt string with callsign.
     * @return Prompt string (e.g., "IU3QEZ> " or "ESP32-Keyer> " if no callsign)
     */
    std::string GetPrompt() const;

    friend class InputHandler;  // Allows InputHandler to call ProcessLine() and GetPrompt()
};

} // namespace ui
