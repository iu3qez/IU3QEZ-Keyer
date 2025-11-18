/**
 * @file morse_encoder.cpp
 * @brief Morse code encoder implementation
 *
 * Implements ITU-standardized morse code encoding table.
 * Provides O(1) character → pattern conversion.
 */

#include "morse_decoder/morse_encoder.hpp"

#include <cctype>
#include "esp_log.h"

namespace morse_decoder {

namespace {
constexpr char kLogTag[] = "morse_encoder";
}  // namespace

MorseEncoder::MorseEncoder() {
  InitializeTable();
  ESP_LOGI(kLogTag, "Morse encoder initialized with %zu character mappings", encoder_table_.size());
}

void MorseEncoder::InitializeTable() {
  // Reserve space for ~60 characters to avoid rehashing
  encoder_table_.reserve(64);

  // ========================================================================
  // LETTERS A-Z (26 patterns)
  // ========================================================================
  encoder_table_['A'] = ".-";
  encoder_table_['B'] = "-...";
  encoder_table_['C'] = "-.-.";
  encoder_table_['D'] = "-..";
  encoder_table_['E'] = ".";
  encoder_table_['F'] = "..-.";
  encoder_table_['G'] = "--.";
  encoder_table_['H'] = "....";
  encoder_table_['I'] = "..";
  encoder_table_['J'] = ".---";
  encoder_table_['K'] = "-.-";
  encoder_table_['L'] = ".-..";
  encoder_table_['M'] = "--";
  encoder_table_['N'] = "-.";
  encoder_table_['O'] = "---";
  encoder_table_['P'] = ".--.";
  encoder_table_['Q'] = "--.-";
  encoder_table_['R'] = ".-.";
  encoder_table_['S'] = "...";
  encoder_table_['T'] = "-";
  encoder_table_['U'] = "..-";
  encoder_table_['V'] = "...-";
  encoder_table_['W'] = ".--";
  encoder_table_['X'] = "-..-";
  encoder_table_['Y'] = "-.--";
  encoder_table_['Z'] = "--..";

  // ========================================================================
  // NUMBERS 0-9 (10 patterns)
  // ========================================================================
  encoder_table_['0'] = "-----";
  encoder_table_['1'] = ".----";
  encoder_table_['2'] = "..---";
  encoder_table_['3'] = "...--";
  encoder_table_['4'] = "....-";
  encoder_table_['5'] = ".....";
  encoder_table_['6'] = "-....";
  encoder_table_['7'] = "--...";
  encoder_table_['8'] = "---..";
  encoder_table_['9'] = "----.";

  // ========================================================================
  // PUNCTUATION & SYMBOLS (16 patterns)
  // ========================================================================
  encoder_table_['.'] = ".-.-.-";  // Period
  encoder_table_[','] = "--..--";  // Comma
  encoder_table_['?'] = "..--..";  // Question mark
  encoder_table_['\''] = ".----."; // Apostrophe
  encoder_table_['!'] = "-.-.--";  // Exclamation mark
  encoder_table_['/'] = "-..-.";   // Slash
  encoder_table_['('] = "-.--.";   // Open parenthesis
  encoder_table_[')'] = "-.--.-";  // Close parenthesis
  encoder_table_['&'] = ".-...";   // Ampersand
  encoder_table_[':'] = "---...";  // Colon
  encoder_table_[';'] = "-.-.-.";  // Semicolon
  encoder_table_['='] = "-...-";   // Equal sign (also prosign BT)
  encoder_table_['+'] = ".-.-.";   // Plus sign (also prosign AR)
  encoder_table_['-'] = "-....-";  // Minus/hyphen
  encoder_table_['_'] = "..--.-";  // Underscore
  encoder_table_['"'] = ".-..-.";  // Quotation mark
  encoder_table_['@'] = ".--.-.";  // At sign

  // ========================================================================
  // PROSIGNS (special morse abbreviations)
  // ========================================================================
  encoder_table_['<'] = "...-.-";  // SK prosign (end of contact)
}

std::string MorseEncoder::Encode(char ch) const {
  // Convert lowercase to uppercase for case-insensitive matching
  char upper_ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

  // Handle space as word separator (no pattern, handled by caller)
  if (upper_ch == ' ') {
    return "";
  }

  // Lookup character in encoding table
  auto it = encoder_table_.find(upper_ch);
  if (it != encoder_table_.end()) {
    ESP_LOGD(kLogTag, "Encode success: '%c' → '%s'", ch, it->second.c_str());
    return it->second;
  }

  // Character not found in table
  ESP_LOGD(kLogTag, "Encode failed: character '%c' (0x%02X) not supported", ch, static_cast<uint8_t>(ch));
  return "";
}

std::vector<std::string> MorseEncoder::EncodeText(const std::string& text) const {
  std::vector<std::string> patterns;
  patterns.reserve(text.length());

  for (char ch : text) {
    std::string pattern = Encode(ch);
    // Include empty patterns for spaces (they represent word gaps)
    patterns.push_back(pattern);
  }

  ESP_LOGI(kLogTag, "Encoded text '%s' → %zu patterns", text.c_str(), patterns.size());
  return patterns;
}

bool MorseEncoder::IsSupported(char ch) const {
  char upper_ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return upper_ch == ' ' || encoder_table_.find(upper_ch) != encoder_table_.end();
}

}  // namespace morse_decoder
