/**
 * @file OutputBuffer.cpp
 * @brief Output buffer implementation for serial console
 */

#include "OutputBuffer.hpp"
#include "tinyusb_cdc_acm.h"

namespace ui {

// USB-CDC port for console (COM7) - already initialized in usb_early_init.cpp
static constexpr tinyusb_cdcacm_itf_t CONSOLE_CDC_PORT = TINYUSB_CDC_ACM_1;

SerialConsole::OutputBuffer::OutputBuffer() = default;

void SerialConsole::OutputBuffer::addLine(const std::string& line) {
    // Only store lines that fit within LINE_LEN
    if (line.length() < LINE_LEN) {
        buffer_[head_] = line;
        head_ = (head_ + 1) % BUF_LINES;
        if (count_ < BUF_LINES) {
            count_++;
        }
    }
}

void SerialConsole::OutputBuffer::dump() const {
    const char* header = "\r\n--- Output Buffer ---\r\n";
    tinyusb_cdcacm_write_queue(CONSOLE_CDC_PORT,
                               reinterpret_cast<const uint8_t*>(header),
                               strlen(header));

    // Calculate starting position for circular buffer
    size_t start = (count_ < BUF_LINES) ? 0 : head_;
    for (size_t i = 0; i < count_; i++) {
        size_t idx = (start + i) % BUF_LINES;
        const auto& line = buffer_[idx];
        tinyusb_cdcacm_write_queue(CONSOLE_CDC_PORT,
                                   reinterpret_cast<const uint8_t*>(line.c_str()),
                                   line.length());
    }

    const char* footer = "--- End Buffer ---\r\n";
    tinyusb_cdcacm_write_queue(CONSOLE_CDC_PORT,
                               reinterpret_cast<const uint8_t*>(footer),
                               strlen(footer));

    tinyusb_cdcacm_write_flush(CONSOLE_CDC_PORT, 0);  // Non-blocking flush
}

} // namespace ui
