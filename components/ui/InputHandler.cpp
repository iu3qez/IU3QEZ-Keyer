/**
 * @file InputHandler.cpp
 * @brief Input handler implementation for serial console
 */

#include "InputHandler.hpp"
#include "tinyusb_cdc_acm.h"
#include <cstring>

namespace ui {

// USB-CDC port for console (COM7) - already initialized in usb_early_init.cpp
static constexpr tinyusb_cdcacm_itf_t CONSOLE_CDC_PORT = TINYUSB_CDC_ACM_1;

SerialConsole::InputHandler::InputHandler(SerialConsole& console)
    : console_(console) {
}

void SerialConsole::InputHandler::init() {
    // USB-CDC is already initialized in usb_early_init.cpp
    // Just reserve buffer and display initial prompt
    inputBuf_.reserve(MAX_LINE);
    write("\r\n" + console_.GetPrompt());  // Dynamic prompt with callsign
}

void SerialConsole::InputHandler::echo(char c) {
    if (echoEnabled_) {
        tinyusb_cdcacm_write_queue(CONSOLE_CDC_PORT, reinterpret_cast<const uint8_t*>(&c), 1);
        tinyusb_cdcacm_write_flush(CONSOLE_CDC_PORT, 0);  // Non-blocking flush
    }
}

void SerialConsole::InputHandler::write(const std::string& str) {
    tinyusb_cdcacm_write_queue(CONSOLE_CDC_PORT,
                               reinterpret_cast<const uint8_t*>(str.c_str()),
                               str.length());
    tinyusb_cdcacm_write_flush(CONSOLE_CDC_PORT, 0);  // Non-blocking flush
}

int SerialConsole::InputHandler::readChar(char& c) {
    size_t rx_size = 0;
    esp_err_t err = tinyusb_cdcacm_read(CONSOLE_CDC_PORT,
                                        reinterpret_cast<uint8_t*>(&c),
                                        1,
                                        &rx_size);

    if (err == ESP_OK && rx_size > 0) {
        return static_cast<int>(rx_size);  // Return number of bytes read
    }
    return 0;  // No data available
}

void SerialConsole::InputHandler::handleBackspace() {
    if (!inputBuf_.empty() && cursorPos_ > 0) {
        if (cursorPos_ == inputBuf_.length()) {
            // Cursor at end, simple case
            inputBuf_.pop_back();
            cursorPos_--;
            write("\b \b");  // Backspace, space, backspace
        } else {
            // Cursor in middle, need to shift characters
            inputBuf_.erase(cursorPos_ - 1, 1);
            cursorPos_--;
            // Redraw from cursor to end + space to clear last char
            write("\b" + inputBuf_.substr(cursorPos_) + " ");
            // Move cursor back to correct position
            moveCursorLeft(inputBuf_.length() - cursorPos_ + 1);
        }
    }
}

void SerialConsole::InputHandler::handleEnter() {
    write("\r\n");

    if (!inputBuf_.empty()) {
        // Save to command history
        history_[histIdx_] = inputBuf_;
        histIdx_ = (histIdx_ + 1) % HISTORY_SIZE;

        // Process command through console
        console_.ProcessLine(inputBuf_);

        inputBuf_.clear();
    }

    // Reset navigation and cursor state
    histNav_ = -1;
    cursorPos_ = 0;

    write(console_.GetPrompt());  // Display dynamic prompt after command execution
}

void SerialConsole::InputHandler::handleCtrlC() {
    inputBuf_.clear();
    histNav_ = -1;
    cursorPos_ = 0;
    write("^C\r\n" + console_.GetPrompt());  // Dynamic prompt with callsign
}

void SerialConsole::InputHandler::process() {
    char c;

    if (readChar(c) > 0) {
        // Skip \n if previous character was \r (CRLF handling)
        if (c == '\n' && lastChar_ == '\r') {
            lastChar_ = c;
            return;
        }

        lastChar_ = c;

        // ESC sequence state machine for arrow keys and other control sequences
        // ANSI escape sequences are multi-byte: ESC [ <letter>
        // Example: Up arrow = ESC (0x1B) + '[' + 'A' (three separate bytes)
        // State machine ensures we process all three bytes before dispatching
        switch (escState_) {
            case EscState::NORMAL:
                if (c == 0x1B) {  // ESC byte received
                    escState_ = EscState::ESC_RECEIVED;
                    return;  // Wait for next byte
                }
                break;

            case EscState::ESC_RECEIVED:
                if (c == '[') {  // CSI (Control Sequence Introducer) received
                    escState_ = EscState::CSI_RECEIVED;
                    return;  // Wait for final byte
                } else {
                    // Invalid sequence (ESC not followed by '['), reset state
                    escState_ = EscState::NORMAL;
                    return;
                }

            case EscState::CSI_RECEIVED:
                // Final byte of ESC sequence determines action
                // Arrow keys: ESC [ A (up), ESC [ B (down), ESC [ C (right), ESC [ D (left)
                escState_ = EscState::NORMAL;  // Always reset state after final byte
                switch (c) {
                    case 'A':  // Up arrow
                        handleUpArrow();
                        return;
                    case 'B':  // Down arrow
                        handleDownArrow();
                        return;
                    case 'C':  // Right arrow
                        handleRightArrow();
                        return;
                    case 'D':  // Left arrow
                        handleLeftArrow();
                        return;
                    default:
                        // Unknown CSI sequence (e.g., Home, End, Page Up/Down), ignore
                        return;
                }
        }

        // Normal character processing
        switch (c) {
            case '\r':
            case '\n':
                handleEnter();
                break;

            case '\b':
            case 0x7F:  // DEL key
                handleBackspace();
                break;

            case 0x03:  // Ctrl+C
                handleCtrlC();
                break;

            case '\t':  // TAB - completion
                handleTab();
                break;

            default:
                // Accept printable ASCII characters
                if (c >= 32 && c <= 126 && inputBuf_.length() < MAX_LINE - 1) {
                    // Insert character at cursor position
                    if (cursorPos_ < inputBuf_.length()) {
                        // Insert in middle of line
                        inputBuf_.insert(cursorPos_, 1, c);
                        cursorPos_++;
                        // Redraw from cursor to end
                        write(inputBuf_.substr(cursorPos_ - 1));
                        // Move cursor back to correct position
                        size_t charsAfter = inputBuf_.length() - cursorPos_;
                        if (charsAfter > 0) {
                            moveCursorLeft(charsAfter);
                        }
                    } else {
                        // Append at end (normal case)
                        inputBuf_ += c;
                        cursorPos_++;
                        echo(c);
                    }
                }
                break;
        }
    }
}

//=============================================================================
// Arrow Key Handlers (History Navigation & Cursor Movement)
//=============================================================================

void SerialConsole::InputHandler::handleUpArrow() {
    // Navigate backwards in history (older commands)
    // History is circular: newest at histIdx_-1, oldest at histIdx_ (wrapping around)

    if (histIdx_ == 0) {
        return;  // No history yet
    }

    if (histNav_ == -1) {
        // First up-arrow press: start navigation from most recent command
        // histNav_ = -1 means we're not currently navigating history
        histNav_ = (histIdx_ - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    } else {
        // Subsequent up-arrow: move to older command
        int8_t prevIdx = (histNav_ - 1 + HISTORY_SIZE) % HISTORY_SIZE;

        // Don't wrap around past the oldest valid entry
        // Stop if we reach histIdx_ (write position) or find empty slot
        if (prevIdx == histIdx_ || history_[prevIdx].empty()) {
            return;
        }
        histNav_ = prevIdx;
    }

    // Load history entry into input buffer and position cursor at end
    if (!history_[histNav_].empty()) {
        inputBuf_ = history_[histNav_];
        cursorPos_ = inputBuf_.length();
        redrawLine();  // Redraw entire line to replace current input
    }
}

void SerialConsole::InputHandler::handleDownArrow() {
    // Navigate forwards in history (newer commands)
    if (histNav_ == -1) {
        return;  // Not navigating history
    }

    int8_t nextIdx = (histNav_ + 1) % HISTORY_SIZE;

    if (nextIdx == histIdx_) {
        // Reached end of history, clear line
        histNav_ = -1;
        inputBuf_.clear();
        cursorPos_ = 0;
        redrawLine();
    } else {
        // Move to newer command
        histNav_ = nextIdx;
        if (!history_[histNav_].empty()) {
            inputBuf_ = history_[histNav_];
            cursorPos_ = inputBuf_.length();
            redrawLine();
        }
    }
}

void SerialConsole::InputHandler::handleLeftArrow() {
    // Move cursor left
    if (cursorPos_ > 0) {
        cursorPos_--;
        write("\b");  // Move cursor back one position
    }
}

void SerialConsole::InputHandler::handleRightArrow() {
    // Move cursor right
    if (cursorPos_ < inputBuf_.length()) {
        write(std::string(1, inputBuf_[cursorPos_]));  // Output character at cursor
        cursorPos_++;
    }
}

//=============================================================================
// TAB Completion
//=============================================================================

void SerialConsole::InputHandler::handleTab() {
    if (inputBuf_.empty()) {
        return;  // Nothing to complete
    }

    // Get registered commands from dispatcher
    std::vector<Command>& commands = console_.GetCommands();

    // Check if we're completing arguments (input contains space) or command verb
    size_t first_space = inputBuf_.find(' ');

    if (first_space != std::string::npos) {
        // Completing arguments - extract command verb
        std::string cmd_verb = inputBuf_.substr(0, first_space);

        // Find the command
        for (const auto& cmd : commands) {
            if (cmd.verb == cmd_verb && cmd.tab_complete) {
                // Call the command's tab completion handler
                std::vector<std::string> matches = cmd.tab_complete(inputBuf_, cursorPos_);

                if (matches.empty()) {
                    return;  // No matches
                }

                if (matches.size() == 1) {
                    // Unique match, complete the argument
                    inputBuf_ = matches[0];
                    cursorPos_ = inputBuf_.length();
                    redrawLine();
                } else {
                    // Multiple matches, show them
                    write("\r\n");
                    write("Possible completions:\r\n");
                    for (const auto& match : matches) {
                        write("  " + match + "\r\n");
                    }
                    // Redisplay prompt and current input
                    write(console_.GetPrompt() + inputBuf_);
                    cursorPos_ = inputBuf_.length();
                }
                return;
            }
        }
        // Command not found or no tab completion handler - do nothing
        return;
    }

    // Completing command verb (original behavior)
    std::vector<std::string> matches;

    for (const auto& cmd : commands) {
        if (cmd.verb.find(inputBuf_) == 0) {  // Prefix match
            matches.push_back(cmd.verb);
        }
    }

    if (matches.empty()) {
        // No matches, do nothing
        return;
    }

    if (matches.size() == 1) {
        // Unique match, complete the command
        inputBuf_ = matches[0];
        cursorPos_ = inputBuf_.length();
        redrawLine();
    } else {
        // Multiple matches, show them
        write("\r\n");
        write("Possible commands:\r\n");
        for (const auto& match : matches) {
            write("  " + match + "\r\n");
        }
        // Redisplay prompt and current input
        write(console_.GetPrompt() + inputBuf_);
        cursorPos_ = inputBuf_.length();
    }
}

//=============================================================================
// Helper Methods
//=============================================================================

void SerialConsole::InputHandler::redrawLine() {
    // Redraw entire input line (used after history navigation or TAB completion)
    // Strategy: carriage return to start, redraw prompt + input, clear remnants, reposition cursor

    // Step 1: Move to start of line and redraw prompt + new input
    write("\r" + console_.GetPrompt() + inputBuf_);

    // Step 2: Clear any remaining characters from previous longer line
    // If previous line was longer than current, old chars would remain visible
    write("    ");  // Extra spaces to overwrite remnants

    // Step 3: Move cursor back to start of INPUT (not start of line - skip prompt)
    // We wrote: prompt + inputBuf_ + 4 spaces
    // Need to go back: inputBuf_.length() + 4 spaces (NOT including prompt length)
    size_t charsAfter = inputBuf_.length() + 4;  // Input + extra spaces
    if (charsAfter > 0) {
        moveCursorLeft(charsAfter);
    }

    // Step 4: Position cursor at desired position (cursorPos_)
    // FIX: Changed condition from < to <= to handle cursor at end
    // When cursorPos_ == inputBuf_.length(), we need to move forward to end
    if (cursorPos_ > 0 && cursorPos_ <= inputBuf_.length()) {
        moveCursorRight(cursorPos_);
    }
}

void SerialConsole::InputHandler::moveCursorLeft(size_t count) {
    // Move cursor left by 'count' positions using backspace characters
    for (size_t i = 0; i < count; i++) {
        write("\b");
    }
}

void SerialConsole::InputHandler::moveCursorRight(size_t count) {
    // Move cursor right by outputting characters from current physical position
    // This assumes cursor is at position 0 (after redrawLine Step 3) and moves it 'count' positions right
    for (size_t i = 0; i < count && i < inputBuf_.length(); i++) {
        write(std::string(1, inputBuf_[i]));
    }
}

} // namespace ui
