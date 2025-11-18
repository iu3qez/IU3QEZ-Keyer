/**
 * @file CommandDispatcher.cpp
 * @brief Command dispatcher implementation for serial console
 */

#include "CommandDispatcher.hpp"
#include <algorithm>

namespace ui {

SerialConsole::CommandDispatcher::CommandDispatcher(SerialConsole& console)
    : console_(console) {
    registerBuiltins();
}

void SerialConsole::CommandDispatcher::registerCommand(const std::string& verb,
                                                       CommandHandler handler,
                                                       const std::string& help,
                                                       TabCompleteHandler tab_complete) {
    commands_.emplace_back(verb, handler, help, tab_complete);
}

void SerialConsole::CommandDispatcher::registerBuiltins() {
    // Register built-in "help" command
    registerCommand("help",
        [this](const std::vector<std::string>& args) {
            return helpHandler(args);
        },
        "help [command] - Show available commands or help for specific command");
}

int SerialConsole::CommandDispatcher::helpHandler(const std::vector<std::string>& args) {
    // Case 1: "help <command>" - Show help for specific command
    if (args.size() >= 2) {
        const std::string& target_cmd = args[1];

        // Search for the command
        auto it = std::find_if(commands_.begin(), commands_.end(),
            [&target_cmd](const Command& cmd) {
                return cmd.verb == target_cmd;
            });

        if (it != commands_.end()) {
            console_.Printf("%s - %s\r\n", it->verb.c_str(), it->help.c_str());
            return 0;
        } else {
            console_.Printf("Unknown command: %s\r\n", target_cmd.c_str());
            console_.Print("Type 'help' to see all available commands.\r\n");
            return -1;
        }
    }

    // Case 2: "help" (no args) - Show all commands
    console_.Print("Available commands:\r\n");

    for (const auto& cmd : commands_) {
        char buf[256];
        snprintf(buf, sizeof(buf), "  %-12s - %s\r\n",
                cmd.verb.c_str(),
                cmd.help.c_str());
        console_.Print(buf);
    }

    console_.Print("\r\nType 'help <command>' for detailed information about a specific command.\r\n");

    return 0;
}

void SerialConsole::CommandDispatcher::dispatch(const std::vector<std::string>& args) {
    if (args.empty()) {
        return;
    }

    const std::string& verb = args[0];

    // Search for command
    auto it = std::find_if(commands_.begin(), commands_.end(),
        [&verb](const Command& cmd) {
            return cmd.verb == verb;
        });

    if (it != commands_.end()) {
        // Execute command handler
        int result = it->handler(args);
        if (result != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Error: %d\r\n", result);
            console_.Print(buf);
        }
    } else {
        console_.Printf("Unknown command: %s\r\n", verb.c_str());
        console_.Print("Type 'help' for available commands.\r\n");
    }
}

} // namespace ui
