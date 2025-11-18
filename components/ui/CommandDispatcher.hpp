/**
 * @file CommandDispatcher.hpp
 * @brief Command dispatcher for serial console - routes commands to handlers
 *
 * Nested class of SerialConsole. Handles:
 * - Command registration
 * - Command lookup and dispatch
 * - Built-in "help" command
 * - Error handling for unknown commands
 */

#pragma once

#include "ui/serial_console.hpp"
#include <vector>
#include <memory>

namespace ui {

class SerialConsole::CommandDispatcher {
public:
    explicit CommandDispatcher(SerialConsole& console);

    void registerCommand(const std::string& verb, CommandHandler handler,
                        const std::string& help, TabCompleteHandler tab_complete = nullptr);
    void dispatch(const std::vector<std::string>& args);

    std::vector<Command>& getCommands() { return commands_; }

private:
    SerialConsole& console_;
    std::vector<Command> commands_;

    void registerBuiltins();
    int helpHandler(const std::vector<std::string>& args);
};

} // namespace ui
