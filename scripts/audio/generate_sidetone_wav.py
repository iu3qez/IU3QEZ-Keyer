#!/usr/bin/env python3
"""Generate a sidetone test waveform and (optionally) analyze its spectrum.

This helper produces a PCM WAV file that mirrors the firmware sidetone settings
so humans can inspect amplitude, fade-in/out, and frequency either with an
external tool (Audacity, Sonic Visualiser) or by using the built-in analyzer.

Example:
  python scripts/audio/generate_sidetone_wav.py --output out.wav \
      --frequency 600 --duration 1.2 --attack 8 --release 8 --analyze
"""

from __future__ import annotations

import argparse
import math
import wave
from pathlib import Path

DEFAULT_SAMPLE_RATE = 48_000
DEFAULT_DURATION_S = 1.2
DEFAULT_ATTACK_MS = 8.0
DEFAULT_RELEASE_MS = 8.0
DEFAULT_FREQUENCY = 600.0
DEFAULT_GAIN = 0.8  # Matches firmware sidetone scaling (approx).
BIT_DEPTH = 16
MAX_AMPLITUDE = (2 ** (BIT_DEPTH - 1)) - 1


def envelope_factor(index: int, total_samples: int, attack_samples: int, release_samples: int) -> float:
    if attack_samples > 0 and index < attack_samples:
        return index / attack_samples
    if release_samples > 0 and index >= total_samples - release_samples:
        return (total_samples - index) / release_samples
    return 1.0


def generate_waveform(
    frequency_hz: float,
    sample_rate: int,
    duration_s: float,
    attack_ms: float,
    release_ms: float,
    gain: float,
) -> list[int]:
    total_samples = int(sample_rate * duration_s)
    attack_samples = int(sample_rate * (attack_ms / 1000.0))
    release_samples = int(sample_rate * (release_ms / 1000.0))

    phase_increment = 2.0 * math.pi * frequency_hz / sample_rate
    phase = 0.0
    data: list[int] = []

    for idx in range(total_samples):
        env = envelope_factor(idx, total_samples, attack_samples, release_samples)
        value = gain * env * math.sin(phase)
        sample = int(max(min(value * MAX_AMPLITUDE, MAX_AMPLITUDE), -MAX_AMPLITUDE))
        data.append(sample)
        phase += phase_increment
    return data


def write_wav(path: Path, samples: list[int], sample_rate: int) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(BIT_DEPTH // 8)
        wav.setframerate(sample_rate)
        frames = b"".join(int(sample).to_bytes(2, byteorder="little", signed=True) for sample in samples)
        wav.writeframes(frames)


def analyze(samples: list[int], sample_rate: int, frequency_hz: float) -> None:
    """Perform a simple single-bin DFT to estimate the carrier magnitude and SNR."""
    total_samples = len(samples)
    if total_samples == 0:
        print("No samples to analyze")
        return

    sin_sum = 0.0
    cos_sum = 0.0
    for idx, sample in enumerate(samples):
        angle = 2.0 * math.pi * frequency_hz * idx / sample_rate
        sin_sum += sample * math.sin(angle)
        cos_sum += sample * math.cos(angle)
    fundamental_mag = (2.0 / total_samples) * math.hypot(sin_sum, cos_sum)

    rms_total = math.sqrt(sum((sample / MAX_AMPLITUDE) ** 2 for sample in samples) / total_samples)
    rms_fundamental = fundamental_mag / MAX_AMPLITUDE / math.sqrt(2.0)
    noise_rms = math.sqrt(max(rms_total ** 2 - rms_fundamental ** 2, 1e-12))
    snr_db = 20.0 * math.log10(rms_fundamental / noise_rms) if noise_rms > 0 else float("inf")

    print("Analysis summary:")
    print(f"  Fundamental magnitude (peak): {fundamental_mag:.1f}")
    print(f"  Fundamental RMS: {rms_fundamental:.4f}")
    print(f"  Total RMS: {rms_total:.4f}")
    print(f"  Estimated SNR: {snr_db:.1f} dB")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("sidetone_test.wav"), help="Output WAV path")
    parser.add_argument("--frequency", type=float, default=DEFAULT_FREQUENCY, help="Tone frequency in Hz")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE, help="Sample rate in Hz")
    parser.add_argument("--duration", type=float, default=DEFAULT_DURATION_S, help="Duration in seconds")
    parser.add_argument("--attack", type=float, default=DEFAULT_ATTACK_MS, help="Attack time in milliseconds")
    parser.add_argument("--release", type=float, default=DEFAULT_RELEASE_MS, help="Release time in milliseconds")
    parser.add_argument("--gain", type=float, default=DEFAULT_GAIN, help="Output gain (0.0-1.0)")
    parser.add_argument("--analyze", action="store_true", help="Compute simple DFT metrics after writing the file")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    samples = generate_waveform(
        frequency_hz=args.frequency,
        sample_rate=args.sample_rate,
        duration_s=args.duration,
        attack_ms=args.attack,
        release_ms=args.release,
        gain=args.gain,
    )
    write_wav(args.output, samples, args.sample_rate)
    print(f"Wrote {len(samples)} samples to {args.output}")
    if args.analyze:
        analyze(samples, args.sample_rate, args.frequency)


if __name__ == "__main__":
    main()
