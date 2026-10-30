#!/usr/bin/env python3
"""Writes the Ren'Py test game's tone (tools/renpy-check.sh): 2 s of 440 Hz, mono, 16 bit, 44100 Hz, as a WAV file, so
that the repository holds no audio file. Usage: tone.py <output.wav>"""
import math
import struct
import sys
import wave

RATE = 44100


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    with wave.open(sys.argv[1], "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * 440 * i / RATE)))
                                 for i in range(2 * RATE)))


if __name__ == "__main__":
    main()
