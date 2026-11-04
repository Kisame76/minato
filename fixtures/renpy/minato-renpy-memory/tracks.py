#!/usr/bin/env python3
"""Writes the Ren'Py memory game's music tracks (tools/renpy-memory.sh): <count> distinct 3 s tones, mono, 16 bit,
22050 Hz WAV files track0.wav ... into <folder>, so that the repository holds no audio file.
Usage: tracks.py <folder> <count>"""
import math
import os
import struct
import sys
import wave

RATE = 22050


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    folder, count = sys.argv[1], int(sys.argv[2])
    for index in range(count):
        with wave.open(os.path.join(folder, f"track{index}.wav"), "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(RATE)
            hertz = 220 + 17 * index
            out.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * hertz * i / RATE)))
                                     for i in range(3 * RATE)))


if __name__ == "__main__":
    main()
