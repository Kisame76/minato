# Compatibility

What runs in Minato today, what is missing, and what will never work. Status as of 2026-11-05. Numbers in
parentheses refer to the records indexed in [DESIGN.md](DESIGN.md).

The goal is that every app whose purpose fits the limits at the end of this page works after its import, without
anyone testing engines one by one, and that an app that cannot work says so at import with every reason at once.
Minato is not there yet. In short: native apps and games, and Java apps built on the parts of Android listed
below, run; many real apps still hit a missing piece of the framework.

## Where the project stands

| Milestone | State |
|---|---|
| M1 library and import of plain APKs | Done |
| M2 native loader: NDK programs on the iPhone print what Android prints, without JIT or debugger | Done |
| M3 graphics, input, audio, frame pacing, and the parity benchmark | Done |
| M4 ART and the framework minimum, test apps and a Godot game on the iPhone | Done |
| M5 all APK formats | Done |
| M6 performance: compiled Java code, per-app profiles | In progress |
| M7 download sources inside the app (optional) | Not started |

## What runs today

**Native apps and games** (a NativeActivity launcher, C or C++ through `native_app_glue`): OpenGL ES 2.0 to 3.1,
Vulkan 1.3 as MoltenVK offers it, touch, hardware keys, AAudio and OpenSL ES output, Choreographer at 60 or 120 Hz,
asset access and orientation changes. An app whose launcher subclasses NativeActivity starts on the Java path, so
the framework calls its native code makes through JNI work. An app whose launcher is `android.app.NativeActivity`
itself still starts without Java, where such calls fail, until ADR-0069 moves it to the Java path too.

**Java and Kotlin apps,** on AOSP's ART, in its fast interpreter and, once an app was optimized after its first
close, as compiled code:

- the activity lifecycle, intents within the app, activity results, launch modes and start flags;
- views drawn in software: text, buttons, check boxes, edit fields, images, scroll views, list views, switches,
  seek and progress bars, the common layouts, `Canvas` and image decoding;
- dialogs, toasts, popups and translucent activities as windows of their own;
- web pages in a `WebView` inside the app, with their cookies and the app's own JavaScript objects;
- `SurfaceView`, `GLSurfaceView`, `TextureView`, `SurfaceTexture` and hardware buffers;
- resources, assets, `SharedPreferences`, SQLite, the app's file paths, OBB files;
- Java cryptography and TLS (Conscrypt with BoringSSL), sockets and DNS;
- the soft keyboard and hardware keys in text fields: the keyboard's height reaches the app as the keyboard inset, and
  the Return key does what Android's keyboard does for the field (a new line, or the field's action such as Done or
  Search);
- game controllers (Xbox, PlayStation, MFi and Switch Pro) with their buttons, sticks, triggers and d-pad as Android 15
  gives a standard pad, and a mouse or trackpad where iOS gives a pointer, with hover, clicks and scrolling;
- sensors (CoreMotion), location (CoreLocation), the microphone and the camera (Android's Camera API), each after
  Minato's own question and iOS's prompt for the app;
- notifications in iOS's Notification Center, with banners for important ones while the app is in front and taps that
  open the app's screen;
- alarms and scheduled jobs while the app's process lives;
- document and photo pickers, sharing, and links that open in a Safari view;
- `AudioTrack`, `SoundPool` (WAV, Ogg Vorbis, MP3, AAC and FLAC), `MediaPlayer`, `MediaExtractor` (also Ogg Opus) and
  `MediaCodec` (H.264 and HEVC video; AAC, MP3, FLAC and Opus audio, Opus with up to 8 channels), audio effects and the
  Visualizer;
- started and bound services and broadcasts inside the app;
- Play Asset Delivery and Play Feature Delivery packs that were imported with the app.

**Speed.** Native code runs directly on the CPU, with small costs for the rewritten instructions. In the parity
benchmark (see [BENCHMARKS.md](BENCHMARKS.md)), a Godot test game reaches parity with its native iOS build in all
six configurations, and Minato's own scene in five of six; the sixth, Vulkan under heavy load at 120 Hz, reached
91 % of the native FPS, and its causes are measured. ES 3.1 through ANGLE's Vulkan backend is slower under heavy
load (56 % of the native FPS in that test). Java code runs on ART's fast interpreter and, once an app was optimized
after its first close, as compiled code, so apps that do much work in Java may still be slower than on a phone.

## APK formats

| Format | State |
|---|---|
| A single `.apk`, including fat APKs (the arm64-v8a libraries are used) | Supported |
| APKs with uncompressed native libraries (`extractNativeLibs="false"`) | Supported |
| Split sets as loose files or a folder (`base.apk` plus `split_*.apk`) | Supported |
| `.apks` from bundletool, SAI or App Manager, and a plain `.zip` of APKs | Supported |
| `.xapk` with its OBB files | Supported |
| Single `.obb` files, assigned to an installed app afterwards | Supported |
| `.apkm` without encryption | Supported |
| Encrypted `.apkm` | Refused with its reason |
| Play Asset Delivery and Feature Delivery packs that came with the import | Supported |
| Packs that only the Play Store would download later | Not supported: nothing talks to the Play Store |
| `.aab` (the developer format) | On the Mac only: `tools/aab-to-apk.sh` turns it into a universal APK |
| APKs with only 32-bit ARM or x86 code | Refused: "Incompatible" |

Splits are chosen as bundletool chooses them for the iPhone: arm64-v8a, the screen's density and the preferred
languages. An APK that needs splits it was not imported with shows "Needs split APKs". An update is an import of a
newer version with the same signer (ADR-0034).

## Engines

Only what the project has evidence for is stated as working; "untested" means that no sample has run yet.

- **Godot 4:** runs. Our Godot 4.7.2 test game runs with Vulkan (the Mobile renderer) and GLES (the Compatibility
  renderer) and reaches parity with its native iOS build.
- **Unity with IL2CPP:** the Unity test APK imports and starts on the iPhone, also with IL2CPP libraries whose atomic
  loops use `x18` (ADR-0020); the native path, ES 3.1 and Vulkan are in place. A large Unity game can still run out
  of memory on the iPhone sooner than on Android: depth and multisample buffers take more graphics memory under Metal,
  which is being worked on.
- **Unity with the Mono backend:** not expected to work. Mono needs a JIT, and Unity's Mono builds for Android are
  32-bit only [inferred].
- **SDL2 and Ren'Py:** SDL 2.30's AAudio and OpenSL ES output paths are checked bit for bit. Our Ren'Py 8.5.3 test game
  starts in the Simulator with its picture and sound, and Minato's Ren'Py memory test game plays 15 minutes on the
  iPhone with at most 850 MB. Ren'Py versions built on Python 2.7 stop at the start, where ctypes asks libffi for
  executable memory; trampolines in Minato's own signed code are in work (ADR-0072).
- **Unreal Engine:** untested. Unreal 5 needs ES 3.2, which is not offered, or Vulkan through MoltenVK.
- **libGDX:** an open-source libGDX game draws with OpenGL ES 2 on the Mac, plays its Ogg Vorbis music through
  MediaPlayer and keeps running, also after a trip to the background; the iPhone is not confirmed yet. Its Java code
  runs compiled after the first close, and `SoundPool` plays WAV, Ogg Vorbis, MP3, AAC and FLAC.
- **Cocos2d-x, axmol, Defold, GameMaker:** untested.
- **Flutter:** an open-source Flutter app starts and keeps running on the Mac; the iPhone is not confirmed yet. Release
  builds compile Dart ahead of time and need no JIT.
- **React Native with Hermes:** untested. **With JavaScriptCore:** JavaScriptCore runs on its interpreter (ADR-0071).
  An open-source app runs its JavaScript and builds its screen on the Mac; it then waits for the network, which the
  Mac's test sandbox withholds. The iPhone is not confirmed yet.
- **LuaJIT (LÖVE and others):** an app whose LuaJIT exports its API runs on LuaJIT's interpreter, as Minato switches
  the compiler off (ADR-0071). An open-source LÖVE game runs on the Mac with its compiler off; the iPhone is not
  confirmed yet. A LuaJIT linked into the app without its exports keeps its compiler, which iOS refuses.
- **Kivy (python-for-android):** imports; Get Info names it. Starting one is not checked yet.
- **.NET, Xamarin, MAUI, MonoGame:** Get Info names the runtime. Mono's JIT can be switched to its interpreter for apps
  built with ahead-of-time code (ADR-0071, planned); CoreCLR apps are not covered.
- **Cordova, Capacitor, HTML5 games:** they are a WebView, which shows its pages inside the app. An open-source
  Capacitor app that asked for Android's SDK extension versions no longer stops at that question.

## The gaps, counted

The compatibility plan lists 22 gaps that stop real apps. Recounted on 2026-11-03: **11 closed, 8 partly closed,
3 that are limits we name.**

**Closed:**

- 4 KB libraries: libraries aligned for 4 KB pages load.
- Second windows: dialogs, toasts, popups and translucent activities.
- Activity results, start flags and launch modes.
- SQLite.
- WebView: pages show inside the app, with cookies, scrolling, typing and the app's own JavaScript objects.
- Speed of Java code: the fast interpreter is the default and an app is compiled once after its first close.
- Missing NDK libraries: the whole API 35 set exists, some as stubs.
- Files, pickers and shared media.
- Widgets and menus: every widget the plan listed is in, with the action bar, the text selection menus and the
  magnifier.
- Intents to iOS: links, mail, messages, calls, maps and shares.
- Play Asset Delivery at run time.
- Time zone data for ICU.

**Closed except for a limit that is named below:**

- Google Play services absent: an app that looks for Google Play services or the Play Store gets the answers an
  Android phone without them gives, and a test compares them with Android 15. Minato never names a store as the
  installer. What an app's own libraries do with those answers is the app's behavior.
- Graphics coverage: ES 3.1, `SurfaceTexture` and hardware buffers. ES 3.2 is not available, and Vulkan features that
  MoltenVK lacks stay missing.

**Partly closed:**

- System services that answer null: many are in, this release added the game, lock screen, USB, download, usage stats,
  storage stats, restrictions, search, biometric, device policy, wallpaper, launcher apps, media session, media router,
  Bluetooth, subscription, Telecom, carrier configuration, NFC, MIDI, media projection and role managers, which answer
  as a phone without that hardware or those permissions does; some of Android 15's service types are still missing.
  The text services manager is there and says no spell checker is enabled: a spell checker session is never opened,
  where Android hands one out.
  The service discovery manager reports a discovery as started and finds nothing, and advertises nothing, as Minato has
  no multicast; Wi-Fi Direct answers every action with an error; the IMS managers are there and answer as a phone
  without IMS does.
- Networking: sockets, DNS, the INTERNET permission and the real network (Wi-Fi state, connectivity changes) are in;
  a sitting on the iPhone has not confirmed all of it.
- Services, broadcasts, jobs, alarms and notifications: all of them work while the app runs. Nothing runs while iOS has
  the app suspended or after it ends; a job the app keeps across restarts comes back at its next start.
- Text input, keys and controllers: the keyboard, its text bar, the Return key, hardware keys and the keyboard's inset
  work. Game controllers reach apps as Android 15's standard pad; rumble, a PlayStation pad's touchpad and its extra
  keys are not passed on. A mouse or trackpad reaches apps where iOS gives a pointer; on the iPhone iOS offers a mouse
  only through AssistiveTouch, which turns it into taps, so apps see touches there.
- Java media and video: `AudioTrack`, `SoundPool` (WAV, Ogg Vorbis, MP3, AAC and FLAC), `MediaPlayer` (also Ogg Vorbis
  and Opus), `MediaExtractor` (also Ogg Opus) and `MediaCodec` for H.264, HEVC, AAC, MP3, FLAC and Opus (up to 8
  channels) are in. `MediaCodec` and `MediaExtractor` have no Vorbis yet. The Equalizer, Bass Boost, Virtualizer and
  Loudness Enhancer change the sound of the `AudioTrack` and AAudio streams of their audio session (session 0: the
  whole mix, which `SoundPool` takes part in) as an approximation of Android's effect bundle, not bit for bit;
  `MediaPlayer` plays through them too, when the tap can take its audio (32-bit float, up to 8 channels, an audio track
  in the file); other formats play unchanged. The Preset Reverb and Environmental Reverb add a tail (decay time,
  high-frequency decay, level) to the streams of their session, also as an approximation; the reflections are not
  played, as in Android's bundle. The Visualizer shows the app's whole mix, `MediaPlayer` included, after the effects
  and the player's volume, also while a player plays at a speed other than 1.
- Runtimes that need a JIT: the import names the engine and refuses an app whose engine library cannot run, and Get Info
  says when an engine needs a JIT, which iOS does not allow (Mono without AOT images, CoreCLR, a LuaJIT the switch
  cannot reach, emulators' recompilers).
- Device features and permissions: sensors, location, the microphone and the camera work. For each app Minato asks
  first, then iOS asks once for Minato, and a no stays a no after the second time, as on Android. The camera works
  through Android's Camera API; Camera2 and CameraX are not in yet.
- Framework breadth: some public types of Android 15's API are still missing from `framework.jar`; this release added 32
  more, and the ones still missing are rare in apps. The text classifier answers as a phone without its model does, and
  Linkify finds web and e-mail addresses and phone numbers.

## What does not work yet

In plain terms, as a user meets it:

- Camera2 and CameraX: the camera works only through Android's older Camera API so far.
- The microphone, the camera, location and notifications are new and not yet checked on the iPhone.
- Jobs, alarms and notifications work only while the app runs: nothing runs while iOS has the app suspended.
- A notification's tap while Minato is closed opens the app at the notification's screen, but extras other than strings,
  numbers and booleans in its content intent are not kept.
- On the iPhone a mouse or trackpad acts as a finger: iOS gives it to apps only through AssistiveTouch.
- OpenGL ES 3.2 is not available, so apps that require it do not start. Some Vulkan features that MoltenVK lacks are
  missing too.
- Audio effects: the reverbs are an approximation of Android's (a tail of four feedback lines; an auxiliary reverb
  acts as an insert on its session), `MediaPlayer` files whose audio the tap cannot take play without the Equalizer,
  Bass Boost, Virtualizer and Loudness Enhancer, and their curves approximate Android's.

## What the library tells you

After an import each app shows one status:

- **Ready:** it can start. "Ready, without libX.so" means a library that the app may need could not be prepared;
  Get Info says why.
- **Needs split APKs:** import the missing splits with the base, or as a set.
- **Incompatible: <reason>:** it cannot run, and Get Info lists every reason the import found.
- **Not a valid APK: <reason>:** the file is damaged or not an APK.

Get Info shows the full report: the libraries and their findings, the engine, SDK levels, graphics requirements and
signatures. A crash names its cause when the app returns to the library.

## Limits that stay

**Never, by design** (ADR-0001, ADR-0002, ADR-0006):

- No CPU emulation, so no x86 and no 32-bit ARM code.
- No JIT in the core path. Runtimes that generate machine code at run time (Mono and .NET, LuaJIT with its JIT on,
  dynamic recompilers in emulators) fail unless they have an interpreter of their own. An optional JIT path
  through StikDebug may come later; it will never be required.
- No Google Play services. They are reported as absent, so push messages, Play Billing, Play Games, Google Sign-In,
  the Maps SDK and Play Integrity do not work, and an app built around one of them fails or degrades.
- No bypassing of Play Integrity, license checks (LVL), anti-cheat or DRM. Apps that enforce them refuse to run,
  and protected video (Widevine) does not play.

**Set by iOS:**

- One Android app at a time, in one process: `android:process` and isolated services run in that process, and
  programs cannot start other programs.
- Nothing runs in the background: services, jobs and alarms run only while the app is in front.
- No system roles: no launchers, input methods, accessibility services, VPN, SMS or dialer apps, overlays or home
  screen widgets.
- No telephony, NFC, USB host or classic Bluetooth, and no multicast or broadcast networking.
- Memory: at most about 5 GB for an app when Minato has the `increased-memory-limit` entitlement, about 2.5 GB
  without it, which is the case for a free account through SideStore (ADR-0012).
- Signed code only: every native library must be converted and signed at import, which takes time once per version.
