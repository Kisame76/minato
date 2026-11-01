# Compatibility

What runs in Minato today, what is missing, and what will never work. Status as of 2026-10-31. Numbers in
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

**Java and Kotlin apps,** on AOSP's ART in the interpreter:

- the activity lifecycle, intents within the app, activity results, launch modes and start flags;
- views drawn in software: text, buttons, check boxes, edit fields, images, scroll views, list views, switches,
  seek and progress bars, the common layouts, `Canvas` and image decoding;
- dialogs, toasts, popups and translucent activities as windows of their own;
- `SurfaceView`, `GLSurfaceView`, `TextureView`, `SurfaceTexture` and hardware buffers;
- resources, assets, `SharedPreferences`, SQLite, the app's file paths, OBB files;
- Java cryptography and TLS (Conscrypt with BoringSSL), sockets and DNS;
- the soft keyboard and hardware keys in text fields: the keyboard's height reaches the app as the keyboard inset, and
  the Return key does what Android's keyboard does for the field (a new line, or the field's action such as Done or
  Search);
- game controllers, a mouse and a trackpad as far as the app is concerned: an app sees the same devices, key and motion
  events, hover, scroll and buttons that Android 15 gives it. The iPhone side that reads a real controller, mouse or
  trackpad is not written yet, so none of them reaches an app today;
- sensors (CoreMotion), location (CoreLocation, after iOS's prompt);
- document and photo pickers, sharing, and links that open in a Safari view;
- `AudioTrack`, `SoundPool` (WAV, Ogg Vorbis, MP3, AAC and FLAC), `MediaPlayer`, `MediaExtractor` and video
  `MediaCodec` (H.264 and HEVC video, AAC audio);
- started and bound services and broadcasts inside the app;
- Play Asset Delivery and Play Feature Delivery packs that were imported with the app.

**Speed.** Native code runs directly on the CPU, with small costs for the rewritten instructions. In the parity
benchmark (see [BENCHMARKS.md](BENCHMARKS.md)), a Godot test game reaches parity with its native iOS build in all
six configurations, and Minato's own scene in five of six; the sixth, Vulkan under heavy load at 120 Hz, reached
91 % of the native FPS, and its causes are measured. ES 3.1 through ANGLE's Vulkan backend is slower under heavy
load (56 % of the native FPS in that test). Java code runs in ART's interpreter today, so apps that do much work in
Java are slower than on a phone until compiled code (ADR-0053) becomes the default.

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
- **Unity with IL2CPP, built with clang 9 or newer:** untested. The native path, ES 3.1 and Vulkan are in place.
- **Unity with IL2CPP, built with an older clang:** the Unity test APK is still "Incompatible", because some of
  its `x18` forms have no island yet (ADR-0020).
- **Unity with the Mono backend:** not expected to work. Mono needs a JIT, and Unity's Mono builds for Android are
  32-bit only [inferred].
- **SDL2 and Ren'Py:** SDL 2.30's AAudio and OpenSL ES output paths are checked bit for bit. Our Ren'Py 8.5.3 test game
  starts in the Simulator with its picture and sound; a run on the iPhone is not confirmed yet.
- **Unreal Engine:** untested. Unreal 5 needs ES 3.2, which is not offered, or Vulkan through MoltenVK.
- **libGDX:** untested. Its Java code runs in the interpreter, and `SoundPool` plays WAV, Ogg Vorbis, MP3, AAC and FLAC.
- **Cocos2d-x, axmol, Defold, GameMaker:** untested.
- **Flutter:** untested. Release builds compile Dart ahead of time and need no JIT.
- **React Native with Hermes:** untested.
- **.NET, Xamarin, MAUI, MonoGame:** not expected to work, since they need a JIT.
- **Cordova, Capacitor, HTML5 games:** not yet. They are a WebView, which shows no web pages yet.

## The gaps, counted

The compatibility plan lists 22 gaps that stop real apps. Recounted on 2026-10-31: **11 closed, 9 partly closed,
2 open.**

**Closed:**

- 4 KB libraries: libraries aligned for 4 KB pages load (ADR-0007).
- Second windows: dialogs, toasts, popups and translucent activities.
- Activity results, start flags and launch modes.
- SQLite.
- Missing NDK libraries: the whole API 35 set exists, some as stubs.
- Files, pickers and shared media (ADR-0057).
- Intents to iOS: links, mail, messages, calls, maps and shares (ADR-0062).
- Graphics coverage: ES 3.1, `SurfaceTexture` and hardware buffers (ADR-0044, ADR-0058). ES 3.2 and Vulkan features
  that MoltenVK lacks stay limits.
- Play Asset Delivery at run time (ADR-0050).
- Time zone data for ICU.
- Google Play services absent: an app that looks for Google Play services or the Play Store gets the answers an Android
  phone without them gives (package lookups, binds and starts, store links, content providers, package contexts, the
  install source), and a test compares them with Android 15 (`gms-absent-test`). Minato never names a store as the
  installer. What an app's own libraries do with those answers is the app's behavior.

**Partly closed:**

- System services that answer null: several batches are in; 125 service types are still missing.
- Networking: sockets, DNS, netlink and the INTERNET permission are in; the check on the real network on the iPhone
  is open (ADR-0054).
- Widgets and menus: the common widgets are in; `Toolbar`, `ActionMenuView`, `MediaController`, `Magnifier` and
  `ZoomButtonsController` are not.
- Services, broadcasts, jobs, alarms and notifications: services and broadcasts work inside the app; jobs and
  alarms are kept but never run, and notifications are not shown.
- Speed of Java code: nterp and compiled code pass their tests but are not the default (ADR-0053).
- Text input, keys and controllers: the keyboard, the Return key, hardware keys and the keyboard's inset work; an app
  sees controllers, a mouse and a trackpad, but the iPhone side that reads them is still to come, so they do not
  reach an app yet; panning the content above the keyboard is not done (ADR-0068).
- Java media and video: `AudioTrack`, `MediaPlayer`, `MediaCodec` for video and `MediaExtractor` are in. `SoundPool`
  arrived after the recount and plays WAV, Ogg Vorbis, MP3, AAC and FLAC (Opus not yet); `MediaCodec` decodes AAC audio,
  other audio codecs come later (ADR-0060).
- Device features and permissions: sensors and location are in; the camera and microphone recording are missing
  (only the microphone's permission path exists, ADR-0064).
- Framework breadth: 577 public types of Android 15's API are still missing from `framework.jar` (297 pure Java, 125
  services, 47 with native code, 81 from modules), down from 704 two weeks earlier.

**Open:**

- WebView: an app that makes one starts, and the WebView answers as an empty one does on Android, but it shows no web
  pages yet: the iPhone's web view behind it is the next step (ADR-0056).
- Runtimes that need a JIT: they are not yet named at import; they fail at run time.

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
