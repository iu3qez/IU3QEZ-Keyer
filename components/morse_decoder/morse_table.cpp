/**
 * @file morse_table.cpp
 * @brief Morse code pattern lookup table implementation
 *
 * Implements ITU-standardized morse code lookup table.
 * Provides O(1) pattern → character conversion.
 */

#include "morse_decoder/morse_table.hpp"

#include "esp_log.h"

namespace morse_decoder {

namespace {
constexpr char kLogTag[] = "morse_table";

// Maximum pattern length per ITU standard (longest code: 6 elements)
constexpr size_t kMaxPatternLength = 6;
}  // namespace

MorseTable::MorseTable() {
  InitializeTable();
  ESP_LOGI(kLogTag, "Morse table initialized with %zu patterns", morse_table_.size());
}

void MorseTable::InitializeTable() {
  // Reserve space for ~60 patterns to avoid rehashing
  morse_table_.reserve(64);

  // ========================================================================
  // LETTERS A-Z (26 patterns)
  // ========================================================================
  morse_table_[".-"]    = 'A';
  morse_table_["-..."]  = 'B';
  morse_table_["-.-."]  = 'C';
  morse_table_["-.."]   = 'D';
  morse_table_["."]     = 'E';
  morse_table_["..-."]  = 'F';
  morse_table_["--."]   = 'G';
  morse_table_["...."]  = 'H';
  morse_table_[".."]    = 'I';
  morse_table_[".---"]  = 'J';
  morse_table_["-.-"]   = 'K';
  morse_table_[".-.."]  = 'L';
  morse_table_["--"]    = 'M';
  morse_table_["-."]    = 'N';
  morse_table_["---"]   = 'O';
  morse_table_[".--."]  = 'P';
  morse_table_["--.-"]  = 'Q';
  morse_table_[".-."]   = 'R';
  morse_table_["..."]   = 'S';
  morse_table_["-"]     = 'T';
  morse_table_["..-"]   = 'U';
  morse_table_["...-"]  = 'V';
  morse_table_[".--"]   = 'W';
  morse_table_["-..-"]  = 'X';
  morse_table_["-.--"]  = 'Y';
  morse_table_["--.."]  = 'Z';

  // ========================================================================
  // NUMBERS 0-9 (10 patterns)
  // ========================================================================
  morse_table_["-----"] = '0';
  morse_table_[".----"] = '1';
  morse_table_["..---"] = '2';
  morse_table_["...--"] = '3';
  morse_table_["....-"] = '4';
  morse_table_["....."] = '5';
  morse_table_["-...."] = '6';
  morse_table_["--..."] = '7';
  morse_table_["---.."] = '8';
  morse_table_["----."] = '9';

  // ========================================================================
  // PUNCTUATION & SYMBOLS (16 patterns)
  // NOTE: '$' excluded (7 elements, exceeds kMaxPatternLength=6)
  // ========================================================================
  morse_table_[".-.-.-"] = '.';   // Period
  morse_table_["--..--"] = ',';   // Comma
  morse_table_["..--.."] = '?';   // Question mark
  morse_table_[".----."] = '\'';  // Apostrophe
  morse_table_["-.-.--"] = '!';   // Exclamation mark
  morse_table_["-..-."]  = '/';   // Slash
  morse_table_["-.--."]  = '(';   // Open parenthesis
  morse_table_["-.--.-"] = ')';   // Close parenthesis
  morse_table_[".-..."]  = '&';   // Ampersand
  morse_table_["---..."] = ':';   // Colon
  morse_table_["-.-.-."] = ';';   // Semicolon
  morse_table_["-...-"]  = '=';   // Equal sign
  morse_table_[".-.-."]  = '+';   // Plus sign (also prosign AR)
  morse_table_["-....-"] = '-';   // Minus/hyphen
  morse_table_["..--.-"] = '_';   // Underscore
  morse_table_[".-..-."] = '"';   // Quotation mark
  //morse_table_["...-..-"] = '$';  // Dollar sign
  morse_table_[".--.-."] = '@';   // At sign

  // ========================================================================
  // PROSIGNS (special morse abbreviations)
  // ========================================================================
  // Note: Some prosigns overlap with punctuation (e.g., AR = '+')
  // We use the ASCII representation for simplicity

  // .-.-.  = AR (end of message) → mapped to '+'
  // ...-.- = SK (end of contact) → use '<' as placeholder
  morse_table_["...-.-"] = '<';   // SK prosign

  // -...-  = BT (pause/break) → mapped to '='
  // Already covered above as '=' sign
}

char MorseTable::Lookup(const std::string& pattern) const {
  // Validation: empty pattern
  if (pattern.empty()) {
    ESP_LOGD(kLogTag, "Lookup failed: empty pattern");
    return '\0';
  }

  // Validation: pattern too long (ITU maximum is 6 elements)
  if (pattern.length() > kMaxPatternLength) {
    ESP_LOGW(kLogTag, "Lookup failed: pattern too long (%zu > %zu): '%s'",
             pattern.length(), kMaxPatternLength, pattern.c_str());
    return '\0';
  }

  // Validation: pattern contains invalid characters (must be '.' or '-')
  for (char c : pattern) {
    if (c != '.' && c != '-') {
      ESP_LOGW(kLogTag, "Lookup failed: invalid character '%c' in pattern '%s'",
               c, pattern.c_str());
      return '\0';
    }
  }

  // Lookup pattern in hash map
  auto it = morse_table_.find(pattern);
  if (it != morse_table_.end()) {
    ESP_LOGD(kLogTag, "Lookup success: '%s' → '%c'", pattern.c_str(), it->second);
    return it->second;
  }

  // Pattern not found in table
  ESP_LOGD(kLogTag, "Lookup failed: pattern '%s' not in table", pattern.c_str());
  return '\0';
}

}  // namespace morse_decoder
