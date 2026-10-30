# Building Minato

Minato is built on a Mac. The build compiles a large part of AOSP (Bionic, ART, the framework, hwui and Skia) as
Android guest code with the NDK, builds ANGLE and MoltenVK from source, and packs everything into one iOS app.
The first full build takes a while and needs room; later builds are incremental.

Tests and reference runs are described in [TESTING.md](TESTING.md), releases in [RELEASING.md](RELEASING.md).

## Requirements

- **A Mac with Apple silicon.** The host tools, the JDK and the macOS test builds are arm64 only. The project is
  developed on macOS 27.
- **Xcode 27.0** with the iOS 27 SDK and an iOS 27 Simulator runtime. Stay on Xcode 27.0's project format until
  XcodeGen supports the format of later Xcode versions.
- **Homebrew packages:** `xcodegen`, `cmake`, `ninja` and `bison`. Bison 3 builds the host `aidl`; macOS ships Bison
  2.3, and Homebrew's is keg-only, so it does not replace the system's.

  ```bash
  brew install xcodegen cmake ninja bison
  ```

- **Python 3.** macOS's `/usr/bin/python3` (3.9) runs almost every script. Fetching ANGLE (`tools/build-angle.sh
  --fetch`) runs depot_tools' `gclient.py`, which needs Python 3.11 or newer.
- **Android NDK r30** (30.0.16248370), by default in `~/Library/Android/sdk/ndk/30.0.16248370`. Set `ANDROID_NDK`
  to use another location.
- **For reference runs only:** the Android emulator, the platform tools and the system image
  `system-images/android-35/default/arm64-v8a` in the Android SDK (`ANDROID_HOME`, by default
  `~/Library/Android/sdk`). Android Studio is not needed. See [TESTING.md](TESTING.md).
- **For the device:** an Apple account signed in to Xcode (Settings, Accounts); a free account is enough. The
  iPhone needs Developer Mode.
- **Disk space:** plan for about 25 GB for `build/` with test outputs, plus the NDK (about 3 GB) and, for reference
  runs, the emulator (about 3 GB). [measured on one machine]

The JDK 17 that builds the Java parts is fetched by the project (see below); no system JDK is needed.

## Third-party sources

Nothing from AOSP or the other upstream projects is committed. `tools/fetch-sources.py` downloads the pinned
sources into `build/sources/` (some into other folders below `build/`) and checks each against the digest in
[`third_party/sources.json`](../third_party/sources.json): a SHA-256 over the sorted file list and each file's
SHA-256, which does not depend on how a server packs its archives. A source that matches its pin is not fetched
again.

`third_party/sources.json` holds several lists, each fetched with `--set`:

- `sources` (the default): Bionic, Scudo, GWP-ASan, arm-optimized-routines and parts of system/core, for
  `tools/build-bionic.sh`.
- `art`: ART, libcore, ICU, BoringSSL, Conscrypt, BouncyCastle, okhttp, the CA certificates, R8 and JDK 17, for
  `tools/build-art.sh`.
- `framework`: frameworks/base and the libraries and tools that the framework scripts read.
- `graphics`: hwui, Skia, minikin, HarfBuzz, FreeType and the image codecs, for `tools/build-hwui.sh`.
- `moltenvk-src`: MoltenVK 1.4.2 and its dependencies, for `tools/build-moltenvk.sh`.

```bash
tools/fetch-sources.py                    # the guest libc's sources
tools/fetch-sources.py --set art          # ART and its libraries, the JDK
tools/fetch-sources.py --set framework
tools/fetch-sources.py --set graphics
tools/fetch-sources.py --set moltenvk-src
```

Most build scripts fetch the lists they need themselves. The AOSP tag is `android-15.0.0_r1` throughout. Other
pinned downloads have scripts of their own: ANGLE (`tools/build-angle.sh --fetch`), bundletool
(`tools/fetch-bundletool.py --fetch`) and, for one benchmark, Godot (`tools/fetch-godot.py --fetch`). Each checks
sizes and digests before it unpacks anything. Licenses of everything adopted are listed in
[THIRD_PARTY.md](../THIRD_PARTY.md); the app carries their texts (`tools/bundle-licenses.py`), and
`tools/license-check.py` fails a built app that lacks one.

## A full build, step by step

The app build embeds the whole guest system, so every piece below must exist before Xcode builds the app; the
build phase `tools/embed-guest-system.sh` names whatever is missing and the script that makes it. In this order:

1. **Graphics libraries:**

   ```bash
   tools/build-angle.sh --fetch      # the pinned ANGLE checkout (network; needs Python 3.11 or newer)
   tools/build-angle.sh --build      # patches, builds all slices, writes build/ANGLE.xcframework
   tools/build-moltenvk.sh           # MoltenVK from source into build/MoltenVK.xcframework and build/moltenvk
   tools/build-front-libs.sh         # the guest's libEGL, libGLESv1_CM, libGLESv2, libGLESv3 and libvulkan
   ```

   `tools/fetch-moltenvk.sh --fetch` puts Khronos's MoltenVK release in the same places instead; since ES 3.1 runs
   on ANGLE's Vulkan backend (ADR-0044), the build from source is the one in use.

2. **Guest libc and ART:**

   ```bash
   tools/build-bionic.sh             # ld-android, libc, libm, libdl, libandroid and the rest into build/bionic
   tools/build-art.sh                # ART, its libraries, dalvikvm and the boot jars into build/art
   ```

3. **Views and the framework:**

   ```bash
   tools/build-hwui.sh               # hwui, Skia and libjnigraphics as guest code into build/hwui
   tools/build-aapt2.sh              # the host aapt2 into build/aapt2
   tools/build-aidl.sh               # the host aidl into build/aidl (needs Homebrew's Bison)
   tools/build-framework-res.sh      # framework-res from AOSP's core/res
   tools/build-framework.sh          # framework.jar
   tools/build-framework-native.sh   # app_process64, libandroid_runtime.so, libandroidfw, libsqlite
   ```

4. **The runtime and the converter, with the Mac's tests:**

   ```bash
   tools/build-native.sh             # NDK fixtures, the macOS build and CTest, then the iOS libraries
   ```

   With `--app` it builds only the part the app links: `build/APKRuntime.xcframework` and
   `build/APKConvert.xcframework` for the device and the Simulator.

5. **The graphics host and the app:** `tools/sim.sh` and `tools/deploy.sh` (below) run
   `tools/build-native.sh --app` and `tools/build-graphics-host.sh` themselves before they call `xcodebuild`.

Each script's header comment explains its inputs, outputs and options in detail.

## The build scripts

**App and project:**

- `tools/gen.sh`: generates `Minato.xcodeproj` from [`project.yml`](../project.yml). The project, `app/Info.plist`
  and `app/Minato.entitlements` are generated and not versioned, so change `project.yml`, not Xcode's settings.
  The development team comes from `$DEVELOPMENT_TEAM` or from Xcode's accounts.
- `tools/sim.sh [--headless] [simulator]`: builds for the Simulator, installs and launches. Without a name it takes
  the newest iPhone Simulator.
- `tools/deploy.sh [device]`: builds for the connected iPhone, installs and launches, then hands the app the
  certificate Xcode signed it with (`tools/hand-over-certificate.sh`), so the app can sign converted code.
- Both take `CONFIGURATION` (Debug or Release), `BUILD_SETTINGS` (extra `xcodebuild` settings) and `INSTALL_ONLY`.

**Native code:**

- `tools/build-native.sh [--app [--release]]`: the CMake projects of `runtime/`, `importer/converter/` and
  `graphics/`. `--release` builds the optimized libraries of the benchmark build.
- `tools/build-fixtures.sh`: the NDK test programs of `fixtures/` and the test apps of `fixtures/apps/`, packed with
  `tools/make-apk.sh`. Java test apps without native code have `tools/build-java-test-app.sh`.
- `tools/build-graphics-host.sh [--release]`: `graphics/host` for the device and the Simulator, as
  `build/MinatoGraphics.xcframework`.

**Guest system and framework:** `tools/build-bionic.sh`, `tools/build-art.sh`, `tools/build-hwui.sh`,
`tools/build-aapt2.sh`, `tools/build-aidl.sh`, `tools/build-framework-res.sh`, `tools/build-framework.sh` and
`tools/build-framework-native.sh`, as in the steps above. The guest builds follow AOSP's own `Android.bp` files
through plan generators in `tools/` (`blueprint.py`, `soong_plan.py` and the `*-plan.py` scripts), so the compiler
flags are the ones Soong would use.

**Graphics:** `tools/build-angle.sh`, `tools/build-moltenvk.sh`, `tools/fetch-moltenvk.sh`,
`tools/build-front-libs.sh`; `tools/build-bench.sh` builds the parity benchmark's scene.

**Formats and packaging:** `tools/make-apk.sh` packs a test app into a signed APK (ADR-0031);
`tools/aab-to-apk.sh <bundle.aab> --out <file.apk>` turns an Android App Bundle into a universal APK with bundletool
(ADR-0051).

**Build phases** that `project.yml` runs inside Xcode: `tools/embed-guest-system.sh` (converts and signs the guest
system into the bundle), `tools/embed-fixtures.sh` (the fixture tests, Debug builds with `APK_EMBED_FIXTURES=YES`
only), `tools/embed-benchmark.sh` (the benchmark, with `APK_EMBED_BENCHMARK=YES` only) and
`tools/bundle-licenses.py`.

## CMake presets

The native code builds with the presets in [`CMakePresets.json`](../CMakePresets.json), into
`build/native/<preset>`:

- `macos`: the runtime, the converter and the graphics host for the Mac, with AddressSanitizer and UBSan, the
  tests, and the host tools `apk-convert` and `apk-run`.
- `ios` and `ios-simulator`: the libraries the app links.
- `ios-release` and `ios-simulator-release`: the same, optimized, for the benchmark build.

```bash
cmake --preset macos && cmake --build --preset macos && ctest --preset macos
```

Warnings are errors for the project's own code (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Werror`).

## Tools on the Mac

After `tools/build-native.sh`:

```bash
swift run --package-path importer/APKKit apk-inspect <apk>     # what the app reads from an APK
tools/apk-check.py <apk>                                        # checks an APK's signatures and contents
build/native/macos/importer/converter/apk-convert inspect <so> # what the loader sees in an ELF file
build/native/macos/importer/converter/apk-convert convert <elf> <dylib> --sign -  # a container, signed ad hoc
build/native/macos/runtime/apk-run [options] <dylib>          # runs a guest program in this process
```

`apk-run` starts a guest program as Android's shell would. `--mount <guest path>=<host path>` puts a host folder
under an Android path, `--libraries <guest folder>=<host folder>` makes a folder's containers the libraries of an
Android folder (for example `/system/lib64`), and `--cwd`, `--env` and `--argv0` set the working folder, the
environment and the program name. Further options play a session on the Mac: a configuration, a lifecycle, a
device profile, scripted sensors and location, and picked files. `APKRT_TRACE_SYSCALLS=1` prints every Linux
syscall with its arguments and result.

## Running in the Simulator

```bash
tools/sim.sh
```

The Simulator is good for the library, imports and Java apps with GLES. It has no Vulkan (MoltenVK is built for
devices only), ANGLE offers ES 3.0 there, and its signature checks and memory limits differ from a device's.

## Deploying to an iPhone

1. Sign in to Xcode with your Apple account and connect the iPhone by cable (or pair it for Wi-Fi). Turn on
   Developer Mode on the iPhone and trust the developer profile.
2. Run:

   ```bash
   tools/deploy.sh
   ```

   It builds a Debug build, installs it with `devicectl`, launches it, and hands over the signing certificate, so
   that the app can sign the native code of apps you import. macOS asks once whether to allow the export of the
   certificate from the keychain.
3. With a free account the profile is valid for 7 days; deploy again when it expires.

Signing certificates and private keys never go into the repository (`*.p12` is ignored). Do not uninstall Minato in
a way that removes its data container unless you mean to lose the imported apps and their data.
