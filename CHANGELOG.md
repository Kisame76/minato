# Changelog

All notable changes to Minato are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). How a version is released: `docs/RELEASING.md`.

## Unreleased

### Added

- SoundPool, which many games play their effects through, with WAV, Ogg Vorbis, MP3, AAC and FLAC files
- Ringtones and notification sounds: an app that asks for the default ringtone, notification or alarm sound gets one
  it can play, as on a phone without sounds set, instead of crashing
- Games made with Ren'Py are recognized as such at import

### Changed

- The on-screen keyboard has a button to put it away, as Android's back gesture does while a keyboard is up
- An app that cannot start because a library it needs does not load says why in one line, following the chain of
  libraries to the one that fails ("it needs libpython2.7.so, which cannot load: …")
- An app whose optional library does not load shows "Ready, without" and that library, and starts without it

### Fixed

- Sound no longer plays too fast and choppy in apps that stream it through AAudio or AudioTrack
- Apps lay out their content above the on-screen keyboard instead of under it
