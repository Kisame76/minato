#!/usr/bin/env python3
"""tools/make-tone-samples.sh's source: a 440 Hz sine of half amplitude, 0.5 s, 16-bit, as WAV, at 44100 Hz in
stereo or at the rate and channels given.

Usage: make_tone.py <output.wav> [<rate> <channels>]
"""
import math
import struct
import sys
import wave

SECONDS, FREQUENCY = 0.5, 440.0


def main():
    rate = int(sys.argv[2]) if len(sys.argv) > 2 else 44100
    channels = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    frames = bytearray()
    for i in range(int(rate * SECONDS)):
        value = int(round(0.5 * 32767 * math.sin(2 * math.pi * FREQUENCY * i / rate)))
        frames += struct.pack("<" + "h" * channels, *([value] * channels))
    with wave.open(sys.argv[1], "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))


if __name__ == "__main__":
    main()
