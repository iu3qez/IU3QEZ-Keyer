/**
 * @file test_morse_table.cpp
 * @brief Unit tests for MorseTable class
 *
 * Tests ITU morse code pattern lookup functionality.
 * Coverage: letters, numbers, punctuation, prosigns, error handling.
 */

#include "morse_decoder/morse_table.hpp"
#include "gtest/gtest.h"

using namespace morse_decoder;

namespace {

/**
 * @brief Test fixture for MorseTable tests
 *
 * Creates a fresh MorseTable instance for each test case.
 */
class MorseTableTest : public ::testing::Test {
 protected:
  MorseTable table_;
};

// ============================================================================
// LETTERS A-Z
// ============================================================================

TEST_F(MorseTableTest, LookupCommonLetters) {
  EXPECT_EQ('A', table_.Lookup(".-"));
  EXPECT_EQ('B', table_.Lookup("-..."));
  EXPECT_EQ('C', table_.Lookup("-.-."));
  EXPECT_EQ('D', table_.Lookup("-.."));
  EXPECT_EQ('E', table_.Lookup("."));
}

TEST_F(MorseTableTest, LookupSOSPattern) {
  // SOS: ... --- ...
  EXPECT_EQ('S', table_.Lookup("..."));
  EXPECT_EQ('O', table_.Lookup("---"));
  EXPECT_EQ('S', table_.Lookup("..."));
}

TEST_F(MorseTableTest, LookupAllLetters) {
  EXPECT_EQ('A', table_.Lookup(".-"));
  EXPECT_EQ('B', table_.Lookup("-..."));
  EXPECT_EQ('C', table_.Lookup("-.-."));
  EXPECT_EQ('D', table_.Lookup("-.."));
  EXPECT_EQ('E', table_.Lookup("."));
  EXPECT_EQ('F', table_.Lookup("..-."));
  EXPECT_EQ('G', table_.Lookup("--."));
  EXPECT_EQ('H', table_.Lookup("...."));
  EXPECT_EQ('I', table_.Lookup(".."));
  EXPECT_EQ('J', table_.Lookup(".---"));
  EXPECT_EQ('K', table_.Lookup("-.-"));
  EXPECT_EQ('L', table_.Lookup(".-.."));
  EXPECT_EQ('M', table_.Lookup("--"));
  EXPECT_EQ('N', table_.Lookup("-."));
  EXPECT_EQ('O', table_.Lookup("---"));
  EXPECT_EQ('P', table_.Lookup(".--."));
  EXPECT_EQ('Q', table_.Lookup("--.-"));
  EXPECT_EQ('R', table_.Lookup(".-."));
  EXPECT_EQ('S', table_.Lookup("..."));
  EXPECT_EQ('T', table_.Lookup("-"));
  EXPECT_EQ('U', table_.Lookup("..-"));
  EXPECT_EQ('V', table_.Lookup("...-"));
  EXPECT_EQ('W', table_.Lookup(".--"));
  EXPECT_EQ('X', table_.Lookup("-..-"));
  EXPECT_EQ('Y', table_.Lookup("-.--"));
  EXPECT_EQ('Z', table_.Lookup("--.."));
}

// ============================================================================
// NUMBERS 0-9
// ============================================================================

TEST_F(MorseTableTest, LookupNumbers) {
  EXPECT_EQ('0', table_.Lookup("-----"));
  EXPECT_EQ('1', table_.Lookup(".----"));
  EXPECT_EQ('2', table_.Lookup("..---"));
  EXPECT_EQ('3', table_.Lookup("...--"));
  EXPECT_EQ('4', table_.Lookup("....-"));
  EXPECT_EQ('5', table_.Lookup("....."));
  EXPECT_EQ('6', table_.Lookup("-...."));
  EXPECT_EQ('7', table_.Lookup("--..."));
  EXPECT_EQ('8', table_.Lookup("---.."));
  EXPECT_EQ('9', table_.Lookup("----."));
}

// ============================================================================
// PUNCTUATION
// ============================================================================

TEST_F(MorseTableTest, LookupCommonPunctuation) {
  EXPECT_EQ('.', table_.Lookup(".-.-.-"));  // Period
  EXPECT_EQ(',', table_.Lookup("--..--"));  // Comma
  EXPECT_EQ('?', table_.Lookup("..--.."));  // Question mark
  EXPECT_EQ('!', table_.Lookup("-.-.--"));  // Exclamation
  EXPECT_EQ('/', table_.Lookup("-..-."));   // Slash
}

TEST_F(MorseTableTest, LookupAllPunctuation) {
  EXPECT_EQ('.', table_.Lookup(".-.-.-"));   // Period
  EXPECT_EQ(',', table_.Lookup("--..--"));   // Comma
  EXPECT_EQ('?', table_.Lookup("..--.."));   // Question mark
  EXPECT_EQ('\'', table_.Lookup(".----."));  // Apostrophe
  EXPECT_EQ('!', table_.Lookup("-.-.--"));   // Exclamation
  EXPECT_EQ('/', table_.Lookup("-..-."));    // Slash
  EXPECT_EQ('(', table_.Lookup("-.--."));    // Open paren
  EXPECT_EQ(')', table_.Lookup("-.--.-"));   // Close paren
  EXPECT_EQ('&', table_.Lookup(".-..."));    // Ampersand
  EXPECT_EQ(':', table_.Lookup("---..."));   // Colon
  EXPECT_EQ(';', table_.Lookup("-.-.-."));   // Semicolon
  EXPECT_EQ('=', table_.Lookup("-...-"));    // Equal
  EXPECT_EQ('+', table_.Lookup(".-.-."));    // Plus (AR prosign)
  EXPECT_EQ('-', table_.Lookup("-....-"));   // Minus
  EXPECT_EQ('_', table_.Lookup("..--.-"));   // Underscore
  EXPECT_EQ('"', table_.Lookup(".-..-."));   // Quote
  // NOTE: '$' removed (7 elements, exceeds kMaxPatternLength=6)
  EXPECT_EQ('@', table_.Lookup(".--.-."));   // At sign
}

// ============================================================================
// PROSIGNS
// ============================================================================

TEST_F(MorseTableTest, LookupProsigns) {
  // AR (end of message) = '+' character
  EXPECT_EQ('+', table_.Lookup(".-.-."));

  // SK (end of contact) = '<' character
  EXPECT_EQ('<', table_.Lookup("...-.-"));

  // BT (pause/break) = '=' character
  EXPECT_EQ('=', table_.Lookup("-...-"));
}

// ============================================================================
// ERROR HANDLING
// ============================================================================

TEST_F(MorseTableTest, LookupEmptyPattern) {
  EXPECT_EQ('\0', table_.Lookup(""));
}

TEST_F(MorseTableTest, LookupPatternTooLong) {
  // ITU maximum is 6 elements (e.g., "...-..-" for '$')
  // 7 elements should fail
  EXPECT_EQ('\0', table_.Lookup("......."));
  EXPECT_EQ('\0', table_.Lookup("--------"));
  EXPECT_EQ('\0', table_.Lookup(".-.-.-.-"));
}

TEST_F(MorseTableTest, LookupInvalidCharacters) {
  EXPECT_EQ('\0', table_.Lookup("abc"));
  EXPECT_EQ('\0', table_.Lookup(".-x"));
  EXPECT_EQ('\0', table_.Lookup("123"));
  EXPECT_EQ('\0', table_.Lookup(" "));
  EXPECT_EQ('\0', table_.Lookup(".-\n"));
}

TEST_F(MorseTableTest, LookupUnknownPattern) {
  // Valid dit/dah pattern but not in ITU table
  EXPECT_EQ('\0', table_.Lookup("......"));    // 6 dits (not ITU)
  EXPECT_EQ('\0', table_.Lookup(".---."));     // Not a real pattern
  EXPECT_EQ('\0', table_.Lookup(".-.-"));      // Not in table
}

// ============================================================================
// TABLE SIZE & INITIALIZATION
// ============================================================================

TEST_F(MorseTableTest, TableSizeIsCorrect) {
  // Expected: 26 letters + 10 numbers + 17 punctuation ($removed) + 1 prosign (SK)
  // Total: 54 patterns
  size_t size = table_.Size();
  EXPECT_EQ(54u, size);  // Exactly 54 patterns
}

TEST_F(MorseTableTest, TableIsImmutableAfterConstruction) {
  // Test that multiple lookups return consistent results
  EXPECT_EQ('A', table_.Lookup(".-"));
  EXPECT_EQ('A', table_.Lookup(".-"));
  EXPECT_EQ('A', table_.Lookup(".-"));

  // Test that unknown patterns remain unknown
  EXPECT_EQ('\0', table_.Lookup("......"));
  EXPECT_EQ('\0', table_.Lookup("......"));
}

// ============================================================================
// REAL-WORLD PATTERNS
// ============================================================================

TEST_F(MorseTableTest, DecodeCQPattern) {
  // CQ: -.-. --.-
  EXPECT_EQ('C', table_.Lookup("-.-."));
  EXPECT_EQ('Q', table_.Lookup("--.-"));
}

TEST_F(MorseTableTest, DecodeCQDXPattern) {
  // CQDX: -.-. --.- -.. -..-
  EXPECT_EQ('C', table_.Lookup("-.-."));
  EXPECT_EQ('Q', table_.Lookup("--.-"));
  EXPECT_EQ('D', table_.Lookup("-.."));
  EXPECT_EQ('X', table_.Lookup("-..-"));
}

TEST_F(MorseTableTest, Decode73Pattern) {
  // 73 (best regards): --... ...--
  EXPECT_EQ('7', table_.Lookup("--..."));
  EXPECT_EQ('3', table_.Lookup("...--"));
}

TEST_F(MorseTableTest, DecodeHelloPattern) {
  // HELLO: .... . .-.. .-.. ---
  EXPECT_EQ('H', table_.Lookup("...."));
  EXPECT_EQ('E', table_.Lookup("."));
  EXPECT_EQ('L', table_.Lookup(".-.."));
  EXPECT_EQ('L', table_.Lookup(".-.."));
  EXPECT_EQ('O', table_.Lookup("---"));
}

// ============================================================================
// EDGE CASES
// ============================================================================

TEST_F(MorseTableTest, ShortestPattern) {
  // 'E' and 'T' are the shortest patterns (1 element)
  EXPECT_EQ('E', table_.Lookup("."));
  EXPECT_EQ('T', table_.Lookup("-"));
}

TEST_F(MorseTableTest, LongestPattern) {
  // Longest patterns are 6 elements (kMaxPatternLength=6)
  // Examples: ".-.-.-" (period), "--..--" (comma), "..-.." (question)
  EXPECT_EQ('.', table_.Lookup(".-.-.-"));  // Period (6 elements)
  EXPECT_EQ(',', table_.Lookup("--..--"));  // Comma (6 elements)
  EXPECT_EQ('?', table_.Lookup("..--.."));  // Question mark (6 elements)
}

TEST_F(MorseTableTest, AllDitsPattern) {
  // 5 dits = '5'
  EXPECT_EQ('5', table_.Lookup("....."));

  // 4 dits = 'H' (not '4' which is '....-')
  EXPECT_EQ('H', table_.Lookup("...."));

  // 3 dits = 'S'
  EXPECT_EQ('S', table_.Lookup("..."));

  // 2 dits = 'I'
  EXPECT_EQ('I', table_.Lookup(".."));

  // 1 dit = 'E'
  EXPECT_EQ('E', table_.Lookup("."));
}

TEST_F(MorseTableTest, AllDahsPattern) {
  // 5 dahs = '0'
  EXPECT_EQ('0', table_.Lookup("-----"));

  // 4 dahs = unassigned in ITU
  EXPECT_EQ('\0', table_.Lookup("----"));

  // 3 dahs = 'O'
  EXPECT_EQ('O', table_.Lookup("---"));

  // 2 dahs = 'M'
  EXPECT_EQ('M', table_.Lookup("--"));

  // 1 dah = 'T'
  EXPECT_EQ('T', table_.Lookup("-"));
}

}  // namespace
