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
- Android's action bar for apps with the default theme: the title, the options menu with its overflow, hiding and
  showing it, and action modes, also in apps without an action bar, as on Android 15
- Above the on-screen keyboard, a bar shows the text of the field you type in, with its cursor, also in apps made with
  game engines such as Ren'Py; tap in it to move the cursor
- A hardware keyboard's Menu key opens an app's options menu, the arrow and Enter keys work inside its overflow, and
  the menu follows the app's changes to it
- Apps follow a new screen size, as folding or unfolding the iPhone Duo gives one: apps that handle it get the new
  size, and the others keep theirs and are shown letterboxed, as on Android
- Apps that embed web pages show them inside the app, also pages an app copied into its own storage, and you can tap,
  scroll and type in them; the pages keep their cookies, so logins and settings in them stay, and can call the app's own
  code, as many HTML5 apps and games need, in apps made for Android 4.1 and older all of its public methods, as there;
  deleting the app removes its web data too
- A three-finger swipe down anywhere, over an app's web pages too, opens Minato's menu while an app runs, every time;
  the swipe down from the top edge, which iOS lets through only every other time, still works too
- A tab bar with Home, Library and Settings; the library keeps its search field
- Favorites: mark an app from its menu (touch and hold) or several at once in Select mode, and a small star marks
  them in the library
- Home shows the apps you opened last and your favorites; touch and hold an app there works as in the library
- Clear App Data, in an app's menu and in Get Info: it deletes the app's saved data, such as progress and settings,
  as Android's Clear storage does, and keeps the app and its OBB files
- Settings › Storage lists each app's space, the app and its data apart, largest first
- Report a Problem in Get Info, in an app's menu and in the alerts when an app cannot open or quit unexpectedly: a
  report of the app and its last session, with that session's log, to read and trim first, then a prefilled GitHub
  issue or Share; nothing leaves the iPhone without your tap
- Settings, with the signing certificate, a switch for the FPS display, the version and the acknowledgements
- When iOS closed an app, for example because it used too much memory, or the app ended with an error status, the
  library says so the next time Minato opens

### Changed

- Apps written in Java or Kotlin run faster: Android's fast interpreter is now the default
- A tap outside the on-screen keyboard puts it away, as many iPhone apps do, and does nothing else in the app, so
  games that ask for their keyboard on every tap no longer open it again at once; a drag or a scroll keeps it up, and
  the button above the keyboard is gone
- An app that cannot start because a library it needs does not load says why in one line, following the chain of
  libraries to the one that fails ("it needs libpython2.7.so, which cannot load: …")
- An app whose optional library does not load shows "Ready, without" and that library, and starts without it
- Pulling down in the library shows the search field without a refresh getting in its way; the library still updates
  itself whenever Minato comes to the front
- The signing certificate moved from the library's "…" menu to Settings
- An app's dialogs and other floating windows are centred between the status bar and the home indicator, as Android
  centres them between its bars, instead of in the whole screen

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
- Apps that embed web pages start instead of closing at once
- Apps whose native libraries make raw system calls in an unusual form start instead of being refused at import
- Emoji show in colour, and Arabic, Hebrew, Thai and Hindi text shows instead of boxes
- Text of mixed directions, such as Arabic or Hebrew next to emoji, gets the same width as on Android
- Apps that read the processor's layout, or list its cores as many game engines do, see the iPhone's performance and
  efficiency cores, as on an Android phone, instead of a single core
