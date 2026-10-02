"""AB-1s, decision 5: the two short, quiet sounds gui_build_audio_reactive uses.

What a person has on disk before opening Studio: a sound whose loudness
changes over time. Both are one second of 16-bit mono PCM at 8 kHz; the tone
is 62.5 Hz (the lowest of four bands from 20 Hz to Nyquist) at amplitude 0.1,
-20 dBFS at its peak -- quiet, and short.

  beat.wav          silence for the first half second, the tone after it
  beat_changed.wav  the other way round: the tone first, then silence --
                    the file the scenario copies over the project's sound
                    to make its baked table stale

    python gui_build_audio_reactive.prepare.py --out <folder> --cli <pelican_cli>

(--cli is taken, as the runner passes it, and not used.)
"""

from __future__ import annotations

import argparse
import math
import struct
from pathlib import Path

RATE = 8000
SECONDS = 1.0
FREQUENCY = 62.5
AMPLITUDE = 0.1


def wav(samples: list[float]) -> bytes:
    frames = b"".join(struct.pack("<h", int(round(max(-1.0, min(1.0, s)) * 32767))) for s in samples)
    header = b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    header += b"data" + struct.pack("<I", len(frames))
    return header + frames


def sound(tone_first: bool) -> list[float]:
    count = int(RATE * SECONDS)
    half = count // 2
    samples: list[float] = []
    for index in range(count):
        in_tone = index < half if tone_first else index >= half
        value = AMPLITUDE * math.sin(2.0 * math.pi * FREQUENCY * index / RATE) if in_tone else 0.0
        samples.append(value)
    return samples


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cli", required=False, type=Path)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    (arguments.out / "beat.wav").write_bytes(wav(sound(tone_first=False)))
    (arguments.out / "beat_changed.wav").write_bytes(wav(sound(tone_first=True)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
