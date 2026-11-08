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
- Touching and holding a word in an app's text field selects it and opens the selection's menu, as on Android, also
  while the keyboard is up
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
- The library's "…" menu shows your apps as icons or as a list, sorted by name, date added, last opened or size, or
  in your own order, which you set on iOS 27 and later by dragging an app to another place
- A filter in the library's "…" menu shows All Apps, Favorites, Ready or Not Ready; it stays when you open Minato
  again, and the search works inside it
- With VoiceOver, apps in the library's own order have the actions Move Up, Move Down, Move to Top and Move to
  Bottom, as an alternative to dragging
- Clear App Data, in an app's menu and in Get Info: it deletes the app's saved data, such as progress and settings,
  as Android's Clear storage does, and keeps the app and its OBB files
- Settings › Storage lists each app's space, the app and its data apart, largest first
- Report a Problem in Get Info, in an app's menu and in the alerts when an app cannot open or quit unexpectedly: a
  report of the app and its last session, with that session's log, to read and trim first, then a prefilled GitHub
  issue or Share; nothing leaves the iPhone without your tap
- Settings, with the signing certificate, a switch for the FPS display, the version and the acknowledgements
- A welcome screen at the first launch shows how to add and start apps and how to get back to Minato from an app;
  Settings › How to Use Minato shows it again
- When iOS closed an app, for example because it used too much memory, or the app ended with an error status, the
  library says so the next time Minato opens
- Apps are told when they leave the screen, as Android tells them, so apps that free memory then do so in Minato too,
  and the memory of their windows' images is given back then, as on Android
- Apps written in Java or Kotlin are optimized once, right after you close them the first time ("Optimizing", which
  you can cancel), and their code runs compiled from then on
- Settings › Acknowledgements says where the source code of Minato's GPL-licensed parts is: on each release's page,
  next to the app
- Swipe left and right to move between Home, Library and Settings
- Get Info names the engine of apps made with .NET, LuaJIT or Kivy
- Games that bundle LuaJIT, as many LÖVE games do, run on LuaJIT's interpreter, since iOS does not allow its compiler
- Apps made with React Native and JavaScriptCore run their JavaScript on JavaScriptCore's interpreter, since iOS does
  not allow its compiler
- Apps can save a file or open a folder with Android's document dialogs: Minato asks once, in Android's words,
  whether the app may use its Documents folder, and the app's saves stay there across restarts
- MediaPlayer plays Ogg Vorbis and Opus music, as libGDX games and many others play theirs, from files and from an
  app's own data source
- Get Info says when an app closed itself within 10 seconds of opening, until its next start, and Report a Problem
  names it
- Apps hear of a turn of the screen as on Android: one that does not handle it is restarted with its saved state,
  one that does is told the new size, and an app that asks for landscape or portrait gets it
- Game controllers reach apps: Xbox, PlayStation, MFi and Switch Pro pads connect and disconnect as Android input
  devices, with buttons, sticks, triggers and the d-pad as Android 15 gives a standard pad
- A mouse or trackpad reaches apps as Android 15's mouse, with hover, clicks of all five buttons, dragging with a
  button held, and wheel or two-finger scrolling in both directions, where iOS gives a pointer
- Apps no longer wait forever when they ask for a permission: Minato asks you first for each app, then iOS, remembers
  your answer per app and stops asking after the second no, as Android does
- Location works: apps get precise or approximate positions from the iPhone as Android 15 gives them, and approximate
  apps never see your precise position
- Apps can record with the microphone (AudioRecord, AAudio and OpenSL ES), at the rate and channels they ask for while
  they run in the foreground
- Apps can use the camera through Android's Camera API: preview, preview callbacks, focus and taking pictures, on the
  iPhone's back and front cameras
- Apps' notifications show on iOS: posts, updates and cancels reach Notification Center, with a banner for important
  ones while the app is in front; tapping one opens what the app meant, also while Minato is closed, and apps made for
  Android 12 and older are asked at their first notification channel, as Android 13 does
- Alarms and scheduled jobs run while the app's process lives, at 5 seconds at the earliest as on Android, and jobs an
  app keeps across restarts come back at its next start
- MediaCodec decodes Opus, also surround with up to 8 channels, and MediaExtractor reads Ogg Opus files, so apps with
  their own player, ExoPlayer among them, play Opus
- Audio effects (Equalizer, Bass Boost, Virtualizer, Loudness Enhancer, the reverbs) and the Visualizer can be created
  and set as on Android; the Visualizer shows what the app plays, and the Equalizer, Bass Boost, Virtualizer and
  Loudness Enhancer and the reverbs change the sound of the app's streams of their audio session as an approximation
  of Android's effect bundle, `MediaPlayer` included
- The text magnifier shows what is under your finger while you drag a selection handle
- More of Android's system managers answer as on a phone without the hardware behind them instead of being missing: game
  mode, lock screen, USB, downloads, usage stats, storage stats, app restrictions, search, biometrics, device policy,
  wallpaper and launcher apps, media sessions, the media router, Bluetooth, SIM subscriptions, Telecom, carrier
  configuration, NFC, MIDI, media projection and roles, and 32 more of Android's public classes load
- An app that sets its own language with LocaleManager gets it at once, in its texts and its screens, and keeps it
  at its next start
- Web and e-mail addresses and phone numbers in apps' texts become links where the app asks for them (Linkify), and
  apps that ask for the text classifier get one, as on a phone without its model
- Ren'Py games built on Python 2.7 get past ctypes' callbacks, which needed executable memory, through trampolines in
  Minato's signed code
- Get Info says when an app's engine needs a JIT, which iOS does not allow
- Swiping in from the left or right edge goes back in Android apps, as on Android, and puts the keyboard away first
  when it is up
- Apps whose text fields keep the keyboard up when you tap elsewhere get Apple's key that hides the keyboard in the
  bar above it
- Ogg Vorbis files open in MediaExtractor and decode through MediaCodec, as on Android 15
- Media apps can run a MediaBrowserService with MediaBrowser, MediaSession and MediaController in their own process:
  browsing, the session's state and its transport commands arrive as on Android 15
- BiometricPrompt and BiometricManager: on an iPhone apps find no biometric hardware and take their fallback, as on a
  phone without a fingerprint reader
- Apps and libraries that ask for the fingerprint, grammatical inflection or print manager, the share action provider,
  the quick settings tile service, the DRM client, DropBoxManager, Bluetooth GATT, AndroidX Security's keystore
  classes or the backup helpers find them, with Android 15's answers for a phone without that hardware or service
- Accessibility node, window and service infos, and the accessibility spans of styled text, are Android's own classes
  instead of default answers
- When an app crashes or ends itself, Report a Problem says where it happened

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
- An app pauses while Minato's menu is open and when you leave Minato, as Android pauses an app under the lock
  screen; coming back shows Minato's menu, and Continue goes on with the app
- The library shows 4 apps side by side, as Home does, with icons and names as big as on the iPhone's Home Screen of
  each model
- An app's dialogs and other floating windows are centred between the status bar and the home indicator, as Android
  centres them between its bars, instead of in the whole screen
- The library's tiles show each app's status in one line, and every row has the same height; the reason is in Get Info
- The library's "…" menu shows the view and the sort order as Files does, with the active order's direction under its
  name, and says when Manual cannot move apps
- Minato's own screens follow larger text sizes and read well with VoiceOver
- In game engines' text fields, a tap outside goes to the game and the keyboard stays, as on Android; Return finishes
  the field
- Games that draw with OpenGL ES or Vulkan use less memory: the transparent window over their picture keeps one
  full-screen buffer, 14 to 29 MB less on an iPhone 15 Pro Max
- Apps that allocate many objects of one size, as Ren'Py games do at their start, have twice the room for them
- When an app ends with an error, its session log names the files it could not find and its last output, which
  Report a Problem includes
- Tapping a notification of the app you were using opens what it points to right away, instead of showing Minato's
  menu first
- Selecting apps in the library is quicker, also with many apps: each tap does far less work, and icons are decoded
  once and kept
- Apps with scheduled jobs use less power while they wait: Minato's job runner wakes 2 times a minute instead of 236
- Apps built on NativeActivity that bring their own Java code start with Android's full Java framework, so their calls
  into Java answer as on Android
- Jobs stop as on Android: a job the app cancels, replaces or clears with cancelAll gets Android's stop reason, and a
  running job that loses its charging or battery constraint stops and starts again once it holds
- ActivityOptions keeps the options an app sets and hands them on with Android's keys and values

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
- Apps see the iPhone's network, over Wi-Fi or cellular, and notice when it changes, instead of finding none, so apps
  that wait for the network before they load no longer wait forever; apps that ask whether Wi-Fi is on find it on
  while the iPhone uses Wi-Fi, and the iPhone's Low Data Mode reads as Android's Data Saver
- Apps that ask for a network of a kind, such as any network with Internet or a cellular one, no longer crash, and
  hear only of networks that match
- Android apps' dialogs, popups and toasts show on the iPhone, with the dim behind a dialog, instead of staying
  invisible
- The bar above the keyboard shows what you type in game engines' name fields instead of dots, and shows only while the
  keyboard covers the field
- A tile in the library no longer slides sideways under your finger
- Apps made with Flutter no longer stop at their start for a missing system timer
- Apps that ask for Android's SDK extension versions, as Capacitor apps do, no longer stop at that question
- Kivy apps import instead of being refused for their Python bundle
- Apps that ask for the priority range of a scheduling policy, as LÖVE does at its start, get Linux's answer, and
  apps that ask for a real-time priority for a thread get Android's no instead of an unsupported call
- React Native apps that show images no longer stop at a missing Android contacts class
- Reading a missing file in a folder an app was given access to throws Android's exception instead of a security
  error
- Unity 6 games no longer report that there is not enough storage space: free space is answered as Android does
- Ren'Py 7.8 games that need a lot of memory of one size start where the iPhone has the address space for it
- An app no longer crashes while it closes when the network, the battery or a sensor reports a change at that moment
- /sdcard and Android's other storage links resolve to their real folder in an app's canonical paths, as on Android
- Memory that an app or game engine gives back is really freed, as on Android, so games that load and free a lot no
  longer grow until iOS closes them
- Apps that map a file of their APK or OBB at a 4 KB offset get the mapping instead of an error
- Images that apps load as hardware bitmaps, as most image libraries and ImageDecoder do, are no longer invisible
- Unity games that look up OpenGL ES 3 functions such as glGetStringi in libGLESv2.so no longer crash at their start
- Games that cache their shaders, such as Unity games, no longer crash at their start when a cached shader is damaged;
  the shader is built again, as on Android
- Games with frame pacing, such as Unity games with Optimized Frame Pacing, get Android's EGL fence waits and frame
  timestamps instead of errors
- Unity IL2CPP libraries whose atomic loops use the x18 register import instead of being refused; installed apps are
  converted again at their next start
- A window takes one producer at a time, as on Android: an EGL or Vulkan surface on a window the app drew in software
  is refused, instead of a black window
- A text field at the bottom of an app's screen moves up above the keyboard, as on Android
- A text dialog that asks for the keyboard when it opens, as many games' name dialogs do, gets it
- The delete key works in an empty text field of a game, such as a name field in Ren'Py games
- Apps that check whether they may write to the shared storage root get Android's no, so they use their own folders
  instead of failing later
- Swiping between Home, Library and Settings no longer leaves a black page, and a slow swipe past the middle goes on
  to the next tab
- In Select mode the library's bottom bar (Select All, Favorite, Delete) is no longer hidden behind the tab bar
