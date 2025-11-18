/**
 * @file OutputBuffer.hpp
 * @brief Output buffer for serial console - stores recent output lines
 *
 * Nested class of SerialConsole. Handles:
 * - Buffering recent console output (circular buffer)
 * - Dumping buffer contents for diagnostics
 * - Line length limiting
 *
 * Buffer size: 32 lines × 80 characters = 2.5KB
 */

#pragma once

#include "ui/serial_console.hpp"
#include <array>
#include <string>

namespace ui {

class SerialConsole::OutputBuffer {
public:
    OutputBuffer();

    void addLine(const std::string& line);
    void dump() const;

private:
    static constexpr size_t BUF_LINES = 32;  // Number of lines stored in circular buffer
    static constexpr size_t LINE_LEN = 80;   // Maximum length per line

    std::array<std::string, BUF_LINES> buffer_;
    size_t head_ = 0;   // Next position to write
    size_t count_ = 0;  // Number of lines stored (up to BUF_LINES)
};

} // namespace ui
