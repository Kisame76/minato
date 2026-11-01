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
- AAC, MP3, FLAC and uncompressed audio through MediaCodec, for apps that decode their music and sounds themselves
- Apps already in the library are checked again after a Minato update, so an app a newer Minato can run turns Ready
  without importing it again

### Changed

- A tap outside the on-screen keyboard puts it away, as many iPhone apps do, while a drag or a scroll keeps it up;
  the button above the keyboard is gone
- An app that cannot start because a library it needs does not load says why in one line, following the chain of
  libraries to the one that fails ("it needs libpython2.7.so, which cannot load: …")
- An app whose optional library does not load shows "Ready, without" and that library, and starts without it

### Fixed

- Sound no longer plays too fast and choppy in apps that stream it through AAudio or AudioTrack
- Apps lay out their content above the on-screen keyboard instead of under it
- The Return key works in text fields that leave its action unset, such as game engines' fields, and in apps made for
  very old Android versions
- Apps whose native libraries mix 4 KB and 16 KB page alignment start instead of being refused at import
- An app whose engine library cannot run is marked incompatible, with the reason, instead of starting to a black
  screen
- Apps that ask how they were installed no longer crash
- Dialog-styled screens have their full height again
- Tapping a text field again after hiding the keyboard shows it again in apps that track the keyboard themselves
- Apps that embed web pages start instead of closing at once; their web parts stay empty for now
- Apps whose native libraries make raw system calls in an unusual form start instead of being refused at import
