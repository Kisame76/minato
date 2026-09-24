# Minato

Run Android apps and games (APKs) natively on the iPhone, without an emulator. Private research project, distributed only by sideloading. Previous working title: APK-Runner (ADR-0024).

- Architecture: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- Decisions (ADRs): [`docs/DECISIONS.md`](docs/DECISIONS.md)
- Status and next steps: [`docs/STATUS.md`](docs/STATUS.md)

## Building

Requirements: Xcode 27, XcodeGen, CMake and Ninja (`brew install xcodegen cmake ninja`), Android NDK r30 in `~/Library/Android/sdk/ndk/30.0.16248370` (other location: set `ANDROID_NDK`).

```bash
tools/sim.sh                 # build and run in the Simulator
tools/deploy.sh              # build, install and run on the connected iPhone; hands over the signing certificate on the way
tools/gen.sh                 # only generate the Xcode project, e.g. before opening it in Xcode
tools/build-native.sh --app  # build the C/C++ for the app (build/APKRuntime.xcframework, build/APKConvert.xcframework), before the first build in Xcode
```

`Minato.xcodeproj`, `app/Info.plist` and `app/Minato.entitlements` are generated from [`project.yml`](project.yml) and not versioned. So make changes in `project.yml`, not in Xcode.

## Tests and tools

```bash
swift test --package-path importer/APKKit                      # unit tests (parser and library)
swift run --package-path importer/APKKit apk-inspect <apk>     # shows what the app reads from an APK
tools/apk-check.py <apk>                                        # checks authenticity before an APK is used
tools/build-native.sh                                           # C/C++: NDK fixtures, tests on the Mac (CTest), XCFramework
build/native/macos/importer/converter/apk-convert inspect <so>  # shows what the loader sees in an ELF file, with the findings in its code (TPIDR_EL0, svc, x18)
build/native/macos/importer/converter/apk-convert convert <elf> <dylib> --sign -  # writes the container dylib and signs it ad hoc (unsigned without --sign)
build/native/macos/importer/converter/apk-convert sign <dylib> --sign <p12> --identifier <bundle ID> --chain-from <signed app file>  # signs with a certificate, as the app does on the iPhone
build/native/macos/runtime/apk-run [options] <dylib>            # runs the program in the container (options below)
tools/build-bionic.sh                                           # guest libc: fetches the AOSP sources (7.2 MB, verified), builds libc.so, libm.so, libdl.so, liblog.so, packs the time zones
tools/android-reference.sh                                      # the same tests on the Android emulator, compared with fixtures/expected (ADR-0014)
tools/device-suite.sh                                           # the same tests on the connected iPhone, without a debugger (M2 step 6)
tools/device-suite.sh --sign-on-device                          # likewise, but the app converts and signs itself (M2b, ADR-0022)
tools/device-suite.sh --simulator [--sign-on-device]            # the same runs in the iOS Simulator
tools/hand-over-certificate.sh [--force]                        # hands the signing certificate from the Mac to the app on the iPhone when the app lacks it (deploy.sh calls it)
```

`APKRT_TRACE_SYSCALLS=1` before `apk-run` shows every Linux syscall of the program with arguments and result. Options of `apk-run`:

- `--mount <guest path>=<host path>` puts a host directory under an Android path; otherwise the program sees only `/dev/null`, `/dev/zero` and the random devices.
- `--libraries <guest directory>=<host directory>` makes the containers of a host folder the libraries of an Android folder, e.g. `/system/lib64` for Bionic. The containers next to the program's are in its folder.
- `--cwd <guest path>` sets the working directory, `--env <name>=<value>` adds to the environment, `--argv0 <text>` says how the program was started.

The search is as in linker64: first in `LD_LIBRARY_PATH`, then in `/system/lib64`. The tests start every program like Android's shell, with `--cwd /data/local/tmp --env LD_LIBRARY_PATH=/data/local/tmp --argv0 ./<name>`.

The C/C++ part (`runtime/`, `importer/converter/`) builds with CMake presets ([`CMakePresets.json`](CMakePresets.json)), the NDK test programs in `fixtures/` are built by `tools/build-fixtures.sh`. The test suite of the M2 plan is in `fixtures/suite/`, its expected outputs in `fixtures/expected/`; which tests exist and how each one runs is in `fixtures/tests.txt`. CTest runs every program as if from Android's shell in `/data/local/tmp`, `tools/android-reference.sh` compares the same runs with Android 15 in the emulator (ADR-0014), and `tools/device-suite.sh` runs them on the iPhone: the app then starts once per test with `-runFixture <test>`, the programs are in the bundle as signed containers. With `--sign-on-device` the app converts the ELF files itself and signs them with the certificate in its keychain. `tools/deploy.sh` puts it there by itself: the certificate Xcode signs the app with (ADR-0022). By hand, it works via the menu "…" → "Signing Certificate" with a `.p12` file. The guest libc is AOSP Bionic (`android-15.0.0_r1`): `tools/fetch-sources.py` fetches the sources to `build/sources/` and checks them against [`third_party/sources.json`](third_party/sources.json), `tools/build-bionic.sh` builds it into `build/bionic/lib/`. Third-party code is listed in [`THIRD_PARTY.md`](THIRD_PARTY.md).

Test APKs are stored locally in `apk/` (ignored by Git), see [`docs/TEST-APKS.md`](docs/TEST-APKS.md).

## License

Apache-2.0, see [`LICENSE`](LICENSE).
