# Testing

Minato's yardstick is Android itself. A test app or program must print in Minato exactly what it prints on
Android 15, and the expected output in the repository comes from Android 15's emulator, not from Minato. This page
describes the layers of tests, how to run them and the rules every change follows. Building is described in
[BUILDING.md](BUILDING.md).

## The rules

- **Red first.** Every change starts with a test that fails for the right reason before the change ("red") and
  passes after it ("green"). A test that was never seen failing proves nothing.
- **Android is the reference.** Behavior is compared with Android 15 (AOSP image, arm64-v8a) in the emulator, line
  by line, through the expected files in `fixtures/expected/`. Where Minato departs on purpose, the departure is
  named in the test and in its ADR.
- **Mutations for risky changes.** After a change is green, break it on purpose on a scratch copy of the tree (drop
  a check, flip a condition, use an off-by-one size) and confirm that a test fails each time. A mutant that no test
  catches means a missing test, unless it is shown to be equivalent. This is done by hand for parsers, signing,
  the loader, the rewriter and anything security-related; the count goes into the change's record ("7 of 7
  caught").
- **Both iOS slices.** Code the app links must compile for `iphoneos` and `iphonesimulator`, without warnings. The
  Simulator SDK lacks some APIs the device has (some `MTLDrawable` members, for example), so a device-only build is
  not enough.
- **No skipped tests by accident.** Tests whose inputs are missing are skipped, not failed. "100 % passed" can hide
  a new fixture that never ran, so check CTest's output for `***Skipped` and the check scripts' logs for `skipped`
  after adding a test.
- **The whole suite before a commit** that touches shared code: all of CTest, not a filtered part.

## The layers

### Swift tests (importer and app logic)

```bash
swift test --package-path importer/APKKit
```

The parser (ZIP, binary XML, resources, signatures), the analysis, formats and split selection, the library, the
packer and the session's logic. Many tests feed damaged or hostile input. About 1,200 tests; a few that need
bundletool's real output run in `tools/formats-check.sh`.

### CTest (runtime, converter, graphics, fixtures)

```bash
tools/build-native.sh        # builds the fixtures and the macOS preset, then runs CTest
ctest --preset macos         # CTest alone, after a build
```

The `macos` preset builds with AddressSanitizer and UBSan. Its tests fall into groups by name:

- `converter.*`: the ELF reader, the rewriter, containers and the signer.
- `runtime.*`: the loader, the syscall layer (files, memory, threads, signals, futexes, sockets), the channels,
  audio, media and the hubs for sensors and location, many of them as small guest programs.
- `graphics.*`: the front libraries against ANGLE and MoltenVK on the Mac, and the benchmark's tools.
- `fixture.*`: the NDK test suite end to end. Each program of `fixtures/tests.txt` is converted, signed ad hoc and
  run by `apk-run` in a process of its own, as Android's shell would run it
  (`cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./<program>`), and must print exactly
  `fixtures/expected/<test>.out` and exit with the expected status (`tools/run-fixture.cmake`).

Host-side Python tests of the generators and harnesses run through `tools/framework-host-check.sh`; one runs in
CTest.

### Check scripts (`tools/*-check.sh`)

Most features have a check script: about 165 of them, from `tools/aapt2-check.sh` to
`tools/windows-return-check.sh`. Each builds what it needs, so it never tests an old binary, runs one scenario and
compares the result with Android 15's expected lines. They run in one of three places:

- **The Mac harness:** a Java or native test app runs under `app_process` or `activity-host` through `apk-run`, in
  a session that plays the host's part (display, lifecycle, touches, sensors), with Python drivers in
  `framework/tests/` (`java_app_run.py`, `app_process_run.py`). Most Java framework features are checked here first,
  for example `tools/views-check.sh` (views and their pixels), `tools/sqlite-check.sh` or `tools/ime-check.sh`.
- **The Simulator:** the real app with a Debug build that answers the script's requests
  (`app/Sources/Session/SessionCheck.swift`, `app/Sources/Library/UpdateCheck.swift`). Examples:
  `tools/session-check.sh` (game mode, restart, lifecycle), `tools/set-check.sh` (split sets and containers in the
  library), `tools/java-start-check.sh` (Java apps started from the library).
- **The iPhone:** many scripts take `--device`. They need the iPhone paired and unlocked, with Developer Mode on.

Each script's header comment says what it checks, what it needs first and which options it takes. Scripts that
need a download skip with a message when it is missing; none downloads anything by itself.

### The fixture suite on the iPhone

```bash
tools/device-suite.sh                     # containers signed on the Mac at build time
tools/device-suite.sh --sign-on-device    # the app converts and signs everything itself
tools/device-suite.sh --simulator         # the same runs in the Simulator
tools/device-suite.sh --art               # ART's Java fixtures; --art-run-tests for ART's own run-tests
```

The app is built with the fixtures in its bundle and started once per test, without a debugger; the script
fetches what each program wrote and compares it with `fixtures/expected/`, as CTest does on the Mac.
`--without-increased-memory` signs the build without its entitlements, as SideStore installs it for a free
account.

## Reference logs from Android 15

`tools/android-reference.sh` runs the same programs and test apps on Android 15 in the emulator and compares their
output with `fixtures/expected/`:

```bash
tools/android-reference.sh                  # every test of fixtures/tests.txt
tools/android-reference.sh --app <app>      # one test app, its APK built first
tools/android-reference.sh --art <program>  # a Java program under dalvikvm64 -Xint
tools/android-reference.sh --features       # the image's system features
```

- It creates and starts the AVDs it needs, without a window, and stops them afterwards: `apk-runner-35` (Android
  15 AOSP, arm64-v8a), `apk-runner-35-iphone` with the iPhone 15 Pro Max's display (1290 x 2796 px at density
  480), and `apk-runner-35-bare` without sensors, GPS, modem and cameras, which is closer to what Minato offers.
- Results go to `build/android-reference/`.
- **Expected files:** `<test>.out` and `<test>.err` hold a program's stdout and stderr; `<app>.logcat` holds the
  lines a test app logs under its own tag (`logcat -v tag`). Lines under the tag `<app>-device` differ between
  devices and are shown, not compared.
- **A new test:** write the test app or program, run it on the emulator, read the output, and only then commit it
  as the expected file. Minato's output never becomes the reference.
- The emulator's frame times are not a reference for speed (ADR-0014).
- Run one reference at a time, and let the emulator shut down fully before the next run.

## Speed

Speed is compared with the native iOS build of the same code on the same iPhone, never with the emulator
(ADR-0014, ADR-0037). `tools/benchmark.sh` builds the benchmark build, runs the series on the iPhone (A B B A, five
valid runs per variant) and fetches the CSVs; `tools/bench-summary.py` judges them. An APK passes when it reaches at
least 95 % of the native FPS with a p99 frame time at most 105 % of the native one. Results are in
[BENCHMARKS.md](BENCHMARKS.md).

## Test data

- `fixtures/suite`, `fixtures/static`, `fixtures/dynamic`: NDK test programs.
- `fixtures/apps`: native and Java test apps, packed by `tools/make-apk.sh`.
- `fixtures/java`: the Java sources of the Java test apps and programs.
- `fixtures/expected`: the reference output from Android 15.
- `fixtures/tests.txt`: the list of fixture tests and how each runs.

Every test app is written for this project. APKs of real apps are never committed, and tests and documents never
name them (see [CONTRIBUTING.md](../CONTRIBUTING.md)).
