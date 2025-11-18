/**
 * @file morse_encoder.hpp
 * @brief Morse code encoder - converts text to morse patterns
 *
 * Provides encoding of ASCII text to ITU Morse code patterns.
 * Inverse operation of MorseTable (char → pattern instead of pattern → char).
 *
 * Pattern format: String of '.' (dit) and '-' (dah)
 * Example: 'A' → ".-", 'S' → "...", '0' → "-----"
 *
 * Supports A-Z, 0-9, common punctuation, and prosigns.
 * Unsupported characters return empty string.
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace morse_decoder {

/**
 * @class MorseEncoder
 * @brief Hash-based encoder for converting characters to morse patterns
 *
 * Thread-safe for reads after construction (immutable after initialization).
 * O(1) lookup time complexity via std::unordered_map.
 *
 * Supports:
 * - Letters A-Z (case-insensitive, always uppercase)
 * - Numbers 0-9
 * - Punctuation: . , ? ' ! / ( ) & : ; = + - _ " @ and space
 * - Prosigns: AR (+), SK (<), BT (=)
 *
 * Unsupported characters are skipped during encoding.
 */
class MorseEncoder {
 public:
  /**
   * @brief Constructor - initializes the encoding table
   *
   * Populates the internal hash map with all ITU morse code patterns.
   * Inverse mapping of MorseTable.
   */
  MorseEncoder();

  /**
   * @brief Encode a single character to morse pattern
   *
   * @param ch Character to encode (A-Z, 0-9, punctuation, space)
   * @return Morse pattern string (".-", "...", etc.) or empty if unsupported
   *
   * Examples:
   *   Encode('A')  → ".-"
   *   Encode('a')  → ".-"  (case-insensitive)
   *   Encode('S')  → "..."
   *   Encode('0')  → "-----"
   *   Encode(' ')  → ""    (word space, handled specially)
   *   Encode('#')  → ""    (unsupported character)
   *
   * Thread-safe: Yes (immutable after construction)
   * Time complexity: O(1) average case
   */
  std::string Encode(char ch) const;

  /**
   * @brief Encode a full text string to morse patterns
   *
   * @param text Text to encode (A-Z, 0-9, punctuation)
   * @return Vector of morse patterns, one per character (spaces become empty strings)
   *
   * Examples:
   *   EncodeText("SOS")     → ["...", "---", "..."]
   *   EncodeText("CQ DX")   → ["-.-.", "--.-", "", ".-..", "-..-"]
   *   EncodeText("TEST 73") → ["-", ".", "...", "-", "", "--...", "...--"]
   *
   * Thread-safe: Yes
   * Time complexity: O(n) where n = text length
   */
  std::vector<std::string> EncodeText(const std::string& text) const;

  /**
   * @brief Check if a character is supported
   * @param ch Character to check
   * @return true if character can be encoded, false otherwise
   */
  bool IsSupported(char ch) const;

  /**
   * @brief Get the number of encodable characters
   * @return Number of characters in the encoding table (typically ~60)
   */
  size_t Size() const { return encoder_table_.size(); }

 private:
  /**
   * @brief Initialize the morse code encoding table
   *
   * Populates encoder_table_ with all ITU-standardized patterns.
   * Inverse of MorseTable::InitializeTable().
   * Called by constructor.
   */
  void InitializeTable();

  /**
   * @brief Hash map for character → pattern encoding
   *
   * Key: ASCII character (uppercase)
   * Value: Morse pattern (e.g., ".-")
   *
   * Immutable after InitializeTable() completes.
   */
  std::unordered_map<char, std::string> encoder_table_;
};

}  // namespace morse_decoder
