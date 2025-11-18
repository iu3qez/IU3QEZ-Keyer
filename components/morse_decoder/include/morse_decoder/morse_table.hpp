/**
 * @file morse_table.hpp
 * @brief Morse code pattern lookup table
 *
 * Provides lookup table for ITU Morse code patterns to ASCII characters.
 * Supports A-Z, 0-9, common punctuation, and prosigns.
 *
 * Pattern format: String of '.' (dit) and '-' (dah)
 * Example: ".-" → 'A', "..." → 'S', "-----" → '0'
 *
 * Maximum pattern length: 6 elements (longest ITU code)
 *
 * Invalid patterns return '\0' (null character)
 */

#pragma once

#include <string>
#include <unordered_map>

namespace morse_decoder {

/**
 * @class MorseTable
 * @brief Hash-based lookup table for morse code patterns
 *
 * Thread-safe for reads after construction (immutable after initialization).
 * O(1) lookup time complexity via std::unordered_map.
 *
 * Supports:
 * - Letters A-Z (26 patterns)
 * - Numbers 0-9 (10 patterns)
 * - Punctuation: . , ? ' ! / ( ) & : ; = + - _ " $ @ (17 patterns)
 * - Prosigns: AR (+), SK (<), BT (=), etc.
 *
 * Total: ~60 ITU-standardized morse code patterns
 */
class MorseTable {
 public:
  /**
   * @brief Constructor - initializes the lookup table
   *
   * Populates the internal hash map with all ITU morse code patterns.
   * Called once during MorseDecoder initialization.
   */
  MorseTable();

  /**
   * @brief Lookup a morse pattern and return the corresponding character
   *
   * @param pattern String of '.' and '-' characters (e.g., ".-" for 'A')
   * @return ASCII character if pattern is valid, '\0' if not found
   *
   * Examples:
   *   Lookup(".-")     → 'A'
   *   Lookup("...")    → 'S'
   *   Lookup("-----")  → '0'
   *   Lookup("")       → '\0' (invalid: empty)
   *   Lookup(".......")→ '\0' (invalid: too long)
   *   Lookup("xyz")    → '\0' (invalid: unknown pattern)
   *
   * Thread-safe: Yes (immutable after construction)
   * Time complexity: O(1) average case
   */
  char Lookup(const std::string& pattern) const;

  /**
   * @brief Get the number of patterns in the table
   * @return Number of morse code patterns (typically ~60)
   */
  size_t Size() const { return morse_table_.size(); }

 private:
  /**
   * @brief Initialize the morse code lookup table
   *
   * Populates morse_table_ with all ITU-standardized patterns.
   * Called by constructor.
   *
   * Organization:
   * 1. Letters A-Z (alphabetical order)
   * 2. Numbers 0-9 (ascending order)
   * 3. Punctuation (common symbols)
   * 4. Prosigns (special morse abbreviations)
   */
  void InitializeTable();

  /**
   * @brief Hash map for pattern → character lookup
   *
   * Key: Morse pattern (e.g., ".-")
   * Value: ASCII character (e.g., 'A')
   *
   * Immutable after InitializeTable() completes.
   */
  std::unordered_map<std::string, char> morse_table_;
};

}  // namespace morse_decoder
