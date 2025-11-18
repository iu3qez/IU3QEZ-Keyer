#pragma once

/**
 * @file alaw_codec.hpp
 * @brief A-Law codec (ITU-T G.711) for audio streaming
 *
 * A-Law compression provides 8-bit encoding of 16-bit linear PCM samples
 * with logarithmic quantization optimized for telephony and CW audio.
 *
 * Reference implementation from DL4YHF's Remote CW Keyer.
 *
 * CHARACTERISTICS:
 * - Sample format: 16-bit signed PCM → 8-bit A-Law
 * - Dynamic range: ~13 bits effective (from 16-bit linear)
 * - Distortion: Lower for quiet signals vs μ-Law
 * - Use case: CW sidetone streaming @ 8 kHz over TCP/IP
 *
 * DECOMPRESSION:
 * - Uses lookup table (256 entries) for fast decoding
 * - Single array access: decoded = kALawDecompressTable[alaw_byte]
 * - No function call overhead
 *
 * COMPRESSION:
 * - Uses helper table for exponent calculation
 * - Logarithmic encoding with sign, exponent, mantissa
 */

#include <cstdint>

namespace audio {

/**
 * @brief A-Law decompression lookup table (ITU-T G.711).
 *
 * Maps 8-bit A-Law encoded byte to 16-bit linear PCM sample.
 * Range: -32256 to +32256 (slightly below int16_t max to avoid overflow).
 *
 * Usage:
 *   int16_t pcm_sample = kALawDecompressTable[alaw_byte];
 */
constexpr int16_t kALawDecompressTable[256] = {
    -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736,
    -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784,
    -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
    -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392,
    -22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944,
    -30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136,
    -11008,-10496,-12032,-11520,-8960, -8448, -9984, -9472,
    -15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568,
    -344,  -328,  -376,  -360,  -280,  -264,  -312,  -296,
    -472,  -456,  -504,  -488,  -408,  -392,  -440,  -424,
    -88,   -72,   -120,  -104,  -24,   -8,    -56,   -40,
    -216,  -200,  -248,  -232,  -152,  -136,  -184,  -168,
    -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184,
    -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696,
    -688,  -656,  -752,  -720,  -560,  -528,  -624,  -592,
    -944,  -912,  -1008, -976,  -816,  -784,  -880,  -848,
     5504,  5248,  6016,  5760,  4480,  4224,  4992,  4736,
     7552,  7296,  8064,  7808,  6528,  6272,  7040,  6784,
     2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368,
     3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392,
     22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
     30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136,
     11008, 10496, 12032, 11520, 8960,  8448,  9984,  9472,
     15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
     344,   328,   376,   360,   280,   264,   312,   296,
     472,   456,   504,   488,   408,   392,   440,   424,
     88,    72,   120,   104,    24,     8,    56,    40,
     216,   200,   248,   232,   152,   136,   184,   168,
     1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184,
     1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696,
     688,   656,   752,   720,   560,   528,   624,   592,
     944,   912,  1008,   976,   816,   784,   880,   848
};

/**
 * @brief Decompress A-Law encoded byte to 16-bit linear PCM sample.
 * @param alaw_byte A-Law encoded byte (0-255).
 * @return 16-bit signed PCM sample (range: -32256 to +32256).
 */
inline int16_t ALawDecode(uint8_t alaw_byte) {
  return kALawDecompressTable[alaw_byte];
}

/**
 * @brief Decompress A-Law buffer to 16-bit PCM stereo samples.
 * @param alaw_input A-Law encoded bytes (mono).
 * @param pcm_output Stereo interleaved 16-bit PCM output (left=right).
 * @param num_samples Number of mono samples to decode.
 *
 * Output format: [L0, R0, L1, R1, ...] where L == R (duplicated mono).
 */
inline void ALawDecodeToStereo(const uint8_t* alaw_input, int16_t* pcm_output,
                                size_t num_samples) {
  for (size_t i = 0; i < num_samples; ++i) {
    const int16_t sample = kALawDecompressTable[alaw_input[i]];
    pcm_output[i * 2] = sample;      // Left channel
    pcm_output[i * 2 + 1] = sample;  // Right channel (duplicate)
  }
}

}  // namespace audio
