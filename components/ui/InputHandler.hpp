/**
 * @file InputHandler.hpp
 * @brief Input handler for serial console - processes keyboard input and line editing
 *
 * Nested class of SerialConsole. Handles:
 * - Character input from USB-CDC (TINYUSB_CDC_ACM_1 / COM7)
 * - Line editing (backspace, Ctrl+C)
 * - Command history navigation (arrow keys)
 * - Echo and prompt display
 */

#pragma once

#include "ui/serial_console.hpp"
#include <string>
#include <array>

namespace ui {

class SerialConsole::InputHandler {
public:
    explicit InputHandler(SerialConsole& console);

    void init();
    void process();

private:
    SerialConsole& console_;
    std::string inputBuf_;
    std::array<std::string, HISTORY_SIZE> history_;
    uint8_t histIdx_ = 0;      // Current position for storing new commands
    int8_t histNav_ = -1;      // Current position when navigating history (-1 = not navigating)
    size_t cursorPos_ = 0;     // Cursor position in input buffer (0 = start, inputBuf_.length() = end)
    bool echoEnabled_ = true;
    char lastChar_ = 0;        // Track last character to handle CRLF properly

    // ESC sequence state machine
    enum class EscState {
        NORMAL,       // Not in ESC sequence
        ESC_RECEIVED, // ESC received, waiting for '['
        CSI_RECEIVED  // CSI (ESC[) received, waiting for final byte
    };
    EscState escState_ = EscState::NORMAL;

    void echo(char c);
    void write(const std::string& str);
    void handleBackspace();
    void handleEnter();
    void handleCtrlC();
    void handleUpArrow();
    void handleDownArrow();
    void handleLeftArrow();
    void handleRightArrow();
    void handleTab();
    void redrawLine();                    // Redraw entire input line (for history navigation)
    void moveCursorLeft(size_t count);    // Move cursor left by count positions
    void moveCursorRight(size_t count);   // Move cursor right by count positions
    int readChar(char& c);
};

} // namespace ui
