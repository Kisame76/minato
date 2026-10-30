#!/usr/bin/env bash
# Makes the SoundPool plan's step 3 test files (docs/plans/2026-10-30-soundpool-and-audio-codecs.md): a 440 Hz sine
# of half amplitude, 0.5 s, stereo, 44100 Hz (tools/media/make_tone.py), as tone.mp3 (lame, 128 kbit/s, which macOS
# cannot encode), tone.m4a and tone.aac (AAC in MP4 and in ADTS, afconvert, 128 kbit/s at 44100 Hz), tone.flac and
# tone-alac.m4a (afconvert), tone-ac3.mp4 (AC-3 in MP4, ffmpeg) and tone.aiff (afconvert, 16-bit big endian); and
# the sine in mono at 22050 Hz as tone-mono.mp3 (lame). Which of them Android 15's SoundPool
# decodes is soundpool-codec-test's reference. The encoders' bytes may differ between versions, so the files are
# committed and only remade on purpose.
# Usage: tools/make-tone-samples.sh [<output directory>]   (default: fixtures/apps/soundpool-codec-test/assets)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-fixtures/apps/soundpool-codec-test/assets}"
WORK=build/make-tone-samples
mkdir -p "$WORK" "$OUT"
python3 tools/media/make_tone.py "$WORK/tone.wav"
python3 tools/media/make_tone.py "$WORK/tone-mono.wav" 22050 1
lame --quiet -b 128 "$WORK/tone.wav" "$OUT/tone.mp3"
lame --quiet -b 64 "$WORK/tone-mono.wav" "$OUT/tone-mono.mp3"
afconvert -f m4af -d aac@44100 -b 128000 "$WORK/tone.wav" "$OUT/tone.m4a"
afconvert -f adts -d aac@44100 -b 128000 "$WORK/tone.wav" "$OUT/tone.aac"
afconvert -f flac -d flac "$WORK/tone.wav" "$OUT/tone.flac"
afconvert -f m4af -d alac "$WORK/tone.wav" "$OUT/tone-alac.m4a"
ffmpeg -loglevel error -y -i "$WORK/tone.wav" -c:a ac3 -b:a 128k -f mp4 "$OUT/tone-ac3.mp4"
afconvert -f AIFF -d BEI16 "$WORK/tone.wav" "$OUT/tone.aiff"
