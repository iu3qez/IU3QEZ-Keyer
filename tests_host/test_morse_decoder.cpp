/**
 * @file test_morse_decoder.cpp
 * @brief Unit tests for MorseDecoder class
 *
 * Tests state machine, pattern accumulation, decoding, and buffer management.
 */

#include "morse_decoder/morse_decoder.hpp"
#include "gtest/gtest.h"

using namespace morse_decoder;

namespace {

/**
 * @brief Test fixture for MorseDecoder tests
 *
 * Creates a fresh MorseDecoder instance for each test case.
 */
class MorseDecoderTest : public ::testing::Test {
 protected:
  MorseDecoder decoder_;
};

// ============================================================================
// SIMPLE DECODING - Single Characters
// ============================================================================

TEST_F(MorseDecoderTest, DecodeSingleDit) {
  // Send 'E' (.)
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ('E', decoder_.GetLastChar());
  EXPECT_EQ("E", decoder_.GetDecodedText());
}

TEST_F(MorseDecoderTest, DecodeSingleDah) {
  // Send 'T' (-)
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ('T', decoder_.GetLastChar());
  EXPECT_EQ("T", decoder_.GetDecodedText());
}

TEST_F(MorseDecoderTest, DecodeLetterS) {
  // Send 'S' (...)
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ('S', decoder_.GetLastChar());
  EXPECT_EQ("S", decoder_.GetDecodedText());
}

TEST_F(MorseDecoderTest, DecodeLetterO) {
  // Send 'O' (---)
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ('O', decoder_.GetLastChar());
  EXPECT_EQ("O", decoder_.GetDecodedText());
}

TEST_F(MorseDecoderTest, DecodeLetterA) {
  // Send 'A' (.-)
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ('A', decoder_.GetLastChar());
  EXPECT_EQ("A", decoder_.GetDecodedText());
}

// ============================================================================
// MULTI-CHARACTER DECODING - Words
// ============================================================================

TEST_F(MorseDecoderTest, DecodeSOSPattern) {
  // S: ... (3 dits)
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  // O: --- (3 dahs)
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  // S: ... (3 dits)
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kWordGap);  // Word gap adds space

  std::string decoded = decoder_.GetDecodedText();
  EXPECT_EQ("SOS ", decoded);  // Space added by word gap
  EXPECT_EQ('S', decoder_.GetLastChar());
}

TEST_F(MorseDecoderTest, DecodeCQPattern) {
  // C: -.-. (dah dit dah dit)
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  // Q: --.- (dah dah dit dah)
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ("CQ", decoder_.GetDecodedText());
  EXPECT_EQ('Q', decoder_.GetLastChar());
}

TEST_F(MorseDecoderTest, DecodeHIPattern) {
  // H: .... (4 dits)
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  // I: .. (2 dits)
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ("HI", decoder_.GetDecodedText());
}

// ============================================================================
// WORD GAPS - Space Handling
// ============================================================================

TEST_F(MorseDecoderTest, WordGapAddsSpace) {
  // E: .
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kWordGap);  // Word gap instead of char gap

  // T: -
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  // Word gap adds space after 'E'
  EXPECT_EQ("E T", decoder_.GetDecodedText());
}

TEST_F(MorseDecoderTest, MultipleWordGaps) {
  // A: .-
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kWordGap);

  // B: -...
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kWordGap);

  // C: -.-.
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  // Should be "A B C" with spaces
  EXPECT_EQ("A B C", decoder_.GetDecodedText());
}

// ============================================================================
// STATE MACHINE - Transitions
// ============================================================================

TEST_F(MorseDecoderTest, InitialStateIsIdle) {
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());
  EXPECT_EQ("", decoder_.GetCurrentPattern());
}

TEST_F(MorseDecoderTest, StateTransitionsOnDit) {
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());

  decoder_.ProcessEvent(KeyEvent::kDit);
  EXPECT_EQ(DecoderState::kReceiving, decoder_.GetState());
  EXPECT_EQ(".", decoder_.GetCurrentPattern());

  decoder_.ProcessEvent(KeyEvent::kCharGap);
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());
  EXPECT_EQ("", decoder_.GetCurrentPattern());  // Pattern cleared after finalization
}

TEST_F(MorseDecoderTest, IntraGapDoesNotChangeState) {
  decoder_.ProcessEvent(KeyEvent::kDit);
  EXPECT_EQ(DecoderState::kReceiving, decoder_.GetState());

  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  EXPECT_EQ(DecoderState::kReceiving, decoder_.GetState());  // Still receiving

  decoder_.ProcessEvent(KeyEvent::kDit);
  EXPECT_EQ(DecoderState::kReceiving, decoder_.GetState());
}

TEST_F(MorseDecoderTest, CharGapFinalizesPattern) {
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kDit);
  EXPECT_EQ("..", decoder_.GetCurrentPattern());

  decoder_.ProcessEvent(KeyEvent::kCharGap);
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());
  EXPECT_EQ("", decoder_.GetCurrentPattern());  // Cleared
  EXPECT_EQ('I', decoder_.GetLastChar());  // ".." = 'I'
}

// ============================================================================
// PATTERN ACCUMULATION
// ============================================================================

TEST_F(MorseDecoderTest, PatternAccumulatesDitsAndDahs) {
  decoder_.ProcessEvent(KeyEvent::kDit);
  EXPECT_EQ(".", decoder_.GetCurrentPattern());

  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  EXPECT_EQ(".", decoder_.GetCurrentPattern());

  decoder_.ProcessEvent(KeyEvent::kDah);
  EXPECT_EQ(".-", decoder_.GetCurrentPattern());

  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  EXPECT_EQ(".-", decoder_.GetCurrentPattern());

  decoder_.ProcessEvent(KeyEvent::kDit);
  EXPECT_EQ(".-.", decoder_.GetCurrentPattern());

  decoder_.ProcessEvent(KeyEvent::kCharGap);
  EXPECT_EQ('R', decoder_.GetLastChar());  // ".-." = 'R'
}

// ============================================================================
// ERROR HANDLING - Unknown Patterns
// ============================================================================

TEST_F(MorseDecoderTest, UnknownPatternDecodesAsQuestionMark) {
  // Send 6 dits (invalid pattern, not in ITU table)
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ('?', decoder_.GetLastChar());
  EXPECT_EQ("?", decoder_.GetDecodedText());
}

TEST_F(MorseDecoderTest, PatternOverflowDecodesAsQuestionMark) {
  // Send 7 dits (exceeds kMaxPatternLength = 6)
  for (int i = 0; i < 7; ++i) {
    decoder_.ProcessEvent(KeyEvent::kDit);
    if (i < 6) {
      decoder_.ProcessEvent(KeyEvent::kIntraGap);
    }
  }

  // Pattern overflow should auto-finalize as '?'
  EXPECT_EQ('?', decoder_.GetLastChar());
  EXPECT_EQ("?", decoder_.GetDecodedText());
}

// ============================================================================
// BUFFER MANAGEMENT - Circular Buffer
// ============================================================================

TEST_F(MorseDecoderTest, BufferOverflowDiscardsOldest) {
  // Create decoder with small buffer (3 characters)
  MorseDecoderConfig config;
  config.buffer_size = 3;
  MorseDecoder small_decoder(config);

  // Send 'E' (dit)
  small_decoder.ProcessEvent(KeyEvent::kDit);
  small_decoder.ProcessEvent(KeyEvent::kCharGap);

  // Send 'T' (dah)
  small_decoder.ProcessEvent(KeyEvent::kDah);
  small_decoder.ProcessEvent(KeyEvent::kCharGap);

  // Send 'I' (dit dit)
  small_decoder.ProcessEvent(KeyEvent::kDit);
  small_decoder.ProcessEvent(KeyEvent::kIntraGap);
  small_decoder.ProcessEvent(KeyEvent::kDit);
  small_decoder.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ("ETI", small_decoder.GetDecodedText());

  // Send 'A' (dit dah) - should discard 'E' (oldest)
  small_decoder.ProcessEvent(KeyEvent::kDit);
  small_decoder.ProcessEvent(KeyEvent::kIntraGap);
  small_decoder.ProcessEvent(KeyEvent::kDah);
  small_decoder.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ("TIA", small_decoder.GetDecodedText());  // 'E' discarded
  EXPECT_EQ('A', small_decoder.GetLastChar());
}

// ============================================================================
// ENABLE/DISABLE
// ============================================================================

TEST_F(MorseDecoderTest, InitiallyEnabled) {
  EXPECT_TRUE(decoder_.IsEnabled());
}

TEST_F(MorseDecoderTest, DisabledDecoderIgnoresEvents) {
  decoder_.SetEnabled(false);
  EXPECT_FALSE(decoder_.IsEnabled());

  // Send 'E' (dit) - should be ignored
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ('\0', decoder_.GetLastChar());  // No character decoded
  EXPECT_EQ("", decoder_.GetDecodedText());
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());
}

TEST_F(MorseDecoderTest, ReEnablingDecoderResumesDecoding) {
  decoder_.SetEnabled(false);

  // Send 'E' (ignored)
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  decoder_.SetEnabled(true);
  EXPECT_TRUE(decoder_.IsEnabled());

  // Send 'T' (should work)
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ('T', decoder_.GetLastChar());
  EXPECT_EQ("T", decoder_.GetDecodedText());  // Only 'T', not 'E'
}

// ============================================================================
// RESET
// ============================================================================

TEST_F(MorseDecoderTest, ResetClearsPattern) {
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kDit);
  EXPECT_EQ("..", decoder_.GetCurrentPattern());

  decoder_.Reset();

  EXPECT_EQ("", decoder_.GetCurrentPattern());
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());
}

TEST_F(MorseDecoderTest, ResetClearsBuffer) {
  // Decode 'SOS'
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ("SOS", decoder_.GetDecodedText());

  decoder_.Reset();

  EXPECT_EQ("", decoder_.GetDecodedText());
  EXPECT_EQ('\0', decoder_.GetLastChar());
}

// ============================================================================
// EDGE CASES
// ============================================================================

TEST_F(MorseDecoderTest, UnknownEventIgnored) {
  decoder_.ProcessEvent(KeyEvent::kUnknown);
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());
  EXPECT_EQ("", decoder_.GetCurrentPattern());
}

TEST_F(MorseDecoderTest, GapsInIdleStateIgnored) {
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());

  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());

  decoder_.ProcessEvent(KeyEvent::kCharGap);
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());

  decoder_.ProcessEvent(KeyEvent::kWordGap);
  EXPECT_EQ(DecoderState::kIdle, decoder_.GetState());

  EXPECT_EQ("", decoder_.GetDecodedText());
}

TEST_F(MorseDecoderTest, ConsecutiveCharGapsIgnored) {
  // Decode 'E'
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ("E", decoder_.GetDecodedText());

  // Send another CharGap (should be ignored, no pattern to finalize)
  decoder_.ProcessEvent(KeyEvent::kCharGap);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ("E", decoder_.GetDecodedText());  // No change
}

// ============================================================================
// NUMBERS - Decode Numeric Patterns
// ============================================================================

TEST_F(MorseDecoderTest, DecodeNumber5) {
  // 5: ..... (5 dits)
  for (int i = 0; i < 5; ++i) {
    decoder_.ProcessEvent(KeyEvent::kDit);
    if (i < 4) {
      decoder_.ProcessEvent(KeyEvent::kIntraGap);
    }
  }
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ('5', decoder_.GetLastChar());
  EXPECT_EQ("5", decoder_.GetDecodedText());
}

TEST_F(MorseDecoderTest, Decode123) {
  // 1: .----
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  // 2: ..---
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  // 3: ...--
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDit);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kIntraGap);
  decoder_.ProcessEvent(KeyEvent::kDah);
  decoder_.ProcessEvent(KeyEvent::kCharGap);

  EXPECT_EQ("123", decoder_.GetDecodedText());
}

// ============================================================================
// CONFIGURATION - Custom Config
// ============================================================================

TEST_F(MorseDecoderTest, CustomBufferSizeWorks) {
  MorseDecoderConfig config;
  config.buffer_size = 5;
  MorseDecoder custom_decoder(config);

  // Decode 'E' 6 times
  for (int i = 0; i < 6; ++i) {
    custom_decoder.ProcessEvent(KeyEvent::kDit);
    custom_decoder.ProcessEvent(KeyEvent::kCharGap);
  }

  // Should keep only last 5 'E's
  EXPECT_EQ("EEEEE", custom_decoder.GetDecodedText());
}

}  // namespace
