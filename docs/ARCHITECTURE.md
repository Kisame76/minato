# Minato: architecture

How Minato runs Android apps on the iPhone, as of 2026-10-30. Decisions are indexed in [DESIGN.md](DESIGN.md) and
cited as ADR numbers; what runs today is in [COMPATIBILITY.md](COMPATIBILITY.md).

Markers: **[verified]** read in a primary source or measured; **[inferred]** our own conclusion; **[unverified]**
a secondary source.

Code comments cite the numbered parts of this file: the platform facts F1 to F18, the risks R1 to R17 and the
sections of chapter 4 (4.3 the loader, 4.5 the syscall layer). Those numbers stay stable.

## 1. Summary

- **No emulator.** Android's arm64 machine code runs directly on the iPhone's CPU. Minato recreates the Android
  environment around it, the way Wine recreates Windows (ADR-0001).
- **Native code is converted and signed at import.** iOS runs only signed code and gives apps no writable executable
  memory (F1). So every `.so` file is checked, a few precisely defined instructions are replaced (thread pointer,
  `svc`, `x18`), the result is packed into a Mach-O dylib (a "container") and signed on the device with the same
  certificate Minato is signed with. Minato's own loader then links it as Android's `linker64` would (ADR-0008,
  ADR-0010, ADR-0022).
- **Two worlds in one process.** The *guest world* runs with the Android ABI: real AOSP Bionic, the app's libraries,
  ART, the Java framework and Minato's NDK libraries. The *host world* runs with the Darwin ABI: the loader, the
  Linux syscall layer, ANGLE, MoltenVK, AVFoundation and UIKit. Guest code reaches host code only through syscalls
  and the front libraries of the NDK APIs (ADR-0009, ADR-0011, ADR-0030).
- **Java runs on real ART.** ART from AOSP is built as guest code and runs the app's DEX code. The framework is
  AOSP's own Java where possible, with system services as objects in the same process (ADR-0004, ADR-0033,
  ADR-0039).
- **One app per process.** Starting an app restarts Minato into that app; quitting restarts into the library
  (ADR-0003, ADR-0032).

## 2. Platform facts that determine the design

- **F1.** iOS runs only signed code. `dynamic-codesigning` is Apple-only, and on A15 and later (TXM) freshly written
  code becomes executable only when an attached debugger writes the page. *So:* all guest code exists as a signed
  file before it runs; a JIT exists only as an optional path through StikDebug. [verified: Apple Platform
  Security; StikJIT's `INTEGRATION.md`]
- **F2.** Library Validation lets a process load libraries signed by the same team. *So:* converted libraries are
  signed with the same (free) team's certificate, and nothing is bypassed. [verified: Apple's documentation; spikes
  S1 and S1b: outside the bundle only with the bundle ID as signature identifier]
- **F3.** XNU overwrites `TPIDR_EL0` with CPU and cluster bits on every context switch; Darwin's thread pointer is
  the read-only `TPIDRRO_EL0`. *So:* Android code that reads its thread pointer (the stack protector, ELF TLS) is
  rewritten at import (ADR-0010). [verified: XNU `pcb.c`; spike S3 on the iPhone]
- **F4.** The kernel zeroes `x18`, except with an Apple-private entitlement. *So:* guest code cannot keep a value in
  `x18`; builds with clang 8 or older use it as a normal register, and the converter moves each use into an island
  (ADR-0020). [verified: XNU `locore.s`; LLVM `AArch64TargetParser.cpp`]
- **F5.** Darwin takes the syscall number from `x16`, Linux from `x8`. *So:* every Linux `svc #0` is replaced at
  import. [verified: XNU `sleh.c`]
- **F6.** arm64 processes have a hard 4 GiB `__PAGEZERO`; nothing can be mapped below 4 GiB. *So:* guest code comes
  in as dylibs, and ART's 32-bit references become offsets into a heap window (ADR-0033). [verified: XNU
  `mach_loader.c`]
- **F7.** iOS uses 16 KB pages; NDK r28 and later align libraries to 16 KB by default. *So:* libraries laid out for
  4 KB pages are shifted inside their container (ADR-0007). [verified: Android's documentation; NDK r28 notes]
- **F8.** Apple's arm64 ABI differs from AAPCS64: varargs always go on the stack, `va_list` is `char *`, the caller
  extends small integers, `long double` is 64 bits wide. *So:* no variadic call crosses the boundary, small integer
  types need thunks, and the guest world keeps the Android ABI (ADR-0011). [verified: Apple's "Writing ARM64 code
  for Apple platforms"]
- **F9.** errno values (54 of 87), signal numbers and flags differ, and so do `stat`, `dirent`, `sigaction`,
  `ucontext`, pthread type sizes and `FILE`. *So:* translation happens at one narrow point, the syscall layer
  (ADR-0009). [verified: Bionic's UAPI headers against the iOS SDK]
- **F10.** Darwin's `CLOCK_MONOTONIC` counts during sleep, Linux's does not. *So:* the guest's `CLOCK_MONOTONIC` is
  Darwin's uptime clock and its `CLOCK_BOOTTIME` is Darwin's `CLOCK_MONOTONIC`. [verified: `clock_gettime(3)`]
- **F11.** iOS apps cannot start subprocesses; app extensions are processes with much smaller memory limits and an
  App ID each. *So:* an Android app runs in Minato's foreground process, and there are no extensions (ADR-0003).
  [verified: Apple's extension documentation]
- **F12.** Measured on an iPhone 15 Pro Max with iOS 27: 3.29 GiB of memory available without
  `increased-memory-limit` and 6.00 GiB with it; 7.84 GiB of address space without, 65.80 GiB with. *So:* the guest
  budget is at most 5 GB with the entitlement and 2.5 GB without it (ADR-0012). [verified: spike S2]
- **F13.** A free Apple account gets profiles valid for 7 days, 3 apps per device, 10 App IDs per 7 days and no
  extended virtual addressing. *So:* one bundle ID, no extensions, a weekly refresh (ADR-0015). [verified: Apple's
  membership comparison]
- **F14.** ANGLE's Metal backend supports OpenGL ES up to 3.0 and lacks `OES_EGL_image_external` and
  `eglPresentationTimeANDROID`. *So:* ES 3.1 runs on ANGLE's Vulkan backend (ADR-0044), and Minato's ANGLE patches
  0010 and 0005 add the two extensions. [verified: ANGLE `DisplayMtl.mm`]
- **F15.** MoltenVK 1.4.2 has no geometry shaders and no transform feedback; its swapchains offer BGRA8, RGBA16F and
  10-bit formats, where Android apps expect RGBA8. *So:* `libvulkan` copies RGBA8 images to BGRA8 at present, and
  apps that need geometry shaders are incompatible. [verified: MoltenVK `MVKDevice.mm`; AOSP `swapchain.cpp`]
- **F16.** The iOS 27 SDK requires a launch screen, and Xcode 27 removed `ld64`. *So:* `UILaunchScreen` is in the
  Info.plist, and Minato writes its Mach-O containers itself (ADR-0008). [verified: iOS 27 and Xcode 27 release
  notes]
- **F17.** Signals work without Mach exception ports: `EXC_BAD_ACCESS` arrives as `SIGSEGV` or `SIGBUS` with
  `si_addr`, and `sigaltstack` is honored. *So:* crash handlers and ART's implicit null and stack checks are
  possible, with a translation between Darwin's `mcontext` and Linux's `ucontext`. [verified: XNU `ux_exception.c`,
  `unix_signal.c`]
- **F18.** dyld on iOS 27 loads a dylib only with `LC_UUID` and a dependency on libSystem, and `codesign` writes
  `LC_CODE_SIGNATURE` right after the load commands. *So:* every container has a content UUID and room after its
  load commands (ADR-0019). [verified: dyld's error messages; the converter's tests]

## 3. Overview

```
Library mode                              Game mode (its own process start, ADR-0003)
------------                              -------------------------------------------
SwiftUI: library, import, Get Info        Overlay: menu (Quit, FPS), keyboard, pickers, sheets
Importer (APKKit):                        GUEST WORLD (Android ABI, signed Mach-O containers)
  parse and check APKs, sets, containers    the app's .so files (rewritten and signed at import)
  compatibility report                      Bionic: libc, libm, libdl; ld-android.so
Converter (APKConvert):                     NDK libraries: libandroid, liblog, libaaudio, libOpenSLES,
  scan and rewrite .so                        libmediandk, libEGL, libGLESv2/v3, libvulkan, libz ...
  build Mach-O containers                   ART, framework.jar, libandroid_runtime, hwui and Skia
  sign with the device's certificate        activity-host or app_process64
                                               |                      |
                                          syscall veneers        front libs -> apkrt_host_call
                                               v                      v
                                          HOST WORLD (Darwin ABI, part of the app binary)
                                            loader and linker (ELF semantics, namespaces, dlopen)
                                            Linux personality: memory, VFS, descriptors, threads,
                                              futex, signals, time, sockets, /proc, properties
                                            channels: vsync, input, audio, sensors, location
                                            ANGLE (Metal, Vulkan) | MoltenVK | CAMetalLayer
                                            AVAudioEngine | VideoToolbox | AVPlayer | CoreMotion
```

**Import** (library mode):

1. "+" or "Open with" hands Minato one file, several files or a folder. Minato clones it into the hidden folder
   `Library/Application Support/incoming/`; the original stays where it is (ADR-0018, ADR-0048).
2. APKKit recognizes the format by content (APK, split set, `.apks`, `.xapk`, `.apkm`, `.zip`, `.obb`), extracts
   containers with streaming and checks, and picks the splits that fit the iPhone (ADR-0048, ADR-0049).
3. It parses ZIP, binary XML, `resources.arsc` and the signature blocks (v1 to v3.1), and reads package, version,
   label, icon, launcher activity, SDK levels, GLES and Vulkan requirements, ABIs and Play dependencies.
4. Every arm64 library goes through the converter's check mode, and its NEEDED names and undefined symbols are bound
   against the guest system's export table; every finding goes into one report (ADR-0055).
5. The version is installed under `Library/Application Support/apps/<package>/`. Before a start, each native library
   is converted and signed with the current certificate; the original ELF stays beside it.

**Start** (game mode):

1. The library writes a launch request and restarts Minato (4.10).
2. The new process reads the request once and sets up the host: memory budget, mounts, signal handlers, the session.
3. It runs a guest program from the bundle's guest system: `app_process64` for an app with Java code, or
   `activity-host` for an app whose launcher activity is `android.app.NativeActivity` itself (until ADR-0069 moves
   those to `app_process64` as well).
4. The loader links the program, Bionic, the NDK libraries and, on the Java path, ART. ART loads `framework.jar` and
   the app's DEX code; the app's native libraries come in through `System.loadLibrary` or NativeActivity.
5. The session drives Android's lifecycle from iOS's events, feeds vsync, input, audio and sensors through channels,
   and shows the app's windows. Quitting ends the process and restarts into the library.

## 4. Components

### 4.1 App (`app/`)

- **Library** (`app/Sources/Library`, `app/Sources/Views`): a grid of installed apps with their status ("Ready",
  "Needs Split APKs", "Incompatible: <reason>" and others), "+" for imports, Get Info with the full report and the
  per-app profile (device profile, frame rate), and the signing certificate view.
- **Session** (`app/Sources/Session`): `GameSession` runs one app; `GameModeView` holds the app's layers and
  Minato's menu; around them `FrameClock` (CADisplayLink), `AudioHost`, `MotionHost`, `BatteryHost`, the presenters
  for pickers, open requests and photo exports, and `Restart`.
- **Signing** (`app/Sources/Signing`): the certificate import from SideStore, AltStore and `tools/deploy.sh`, its
  checks, and the coordinator that converts and signs libraries before a start (ADR-0022, ADR-0023).
- **Logic in Swift packages:** most decisions the app makes live in `importer/APKKit`, where `swift test` covers
  them on the Mac. `APKSession` holds the session's logic: launch requests, lifecycle, touch translation, key and
  text delivery, window order, the restart sequence, and the pick and open queues.
- **Info.plist** (generated from `project.yml`): document types for `.apk`, `.apks`, `.xapk`, `.apkm` and `.obb`; no
  `UIFileSharingEnabled`; `CADisableMinimumFrameDurationOnPhone`; the iOS Game Mode keys. Every build requests
  `increased-memory-limit`.
- **Storage:** `Library/Application Support/apps/<package>/` holds the installed versions and converted libraries,
  `Library/Application Support/data/<package>/` the app's data (see 4.5). `Documents/` stays empty.
- **Debug-only hooks:** Debug builds answer requests from the check scripts (`SessionCheck.swift`,
  `UpdateCheck.swift`) and can carry the fixture tests (`app/Fixtures/FixtureRunner.swift`). Benchmark builds carry
  the parity scene (`app/Benchmark`). Release builds hold neither.

### 4.2 Importer (`importer/`)

- **APKKit** (a Swift package that uses Apple's frameworks only), its main targets:
  - `APKKit`: ZIP with ZIP64 and streaming inflate, binary XML (also with obfuscated attribute names),
    `resources.arsc`, the manifest, signature schemes v1 to v3.1 and the Play source stamp, an ELF probe, and the
    analysis (`APKAnalyzer`, `APKReport`).
  - `APKFormats`: split sets, `.apks` (bundletool's table of contents, SAI), `.xapk`, `.apkm`, OBB names, Android's
    install checks and bundletool's split selection.
  - `APKLibrary`: import units, installation, updates (ADR-0034), the library index and deletion.
  - `APKSigning` (certificate information), `APKPack` with `make-apk` (the packer for test apps, ADR-0031),
    `apk-inspect` (a command-line view of what the app reads from an APK), and `APKBench` (the benchmark's plan).
- **Converter** (`importer/converter/`, C++; the `apk-convert` tool on the Mac and `APKConvert.xcframework` in the
  app): the ELF reader (`elf.cpp`), an arm64 decoder (`a64.cpp`), the rewriter (`rewrite.cpp`, 4.4), the container
  writer (`container.cpp`, `macho.cpp`), and a check mode that lists every reason a library is refused.
- **Signer** (in the converter: `der.cpp`, `certificate.cpp`, `pkcs12.cpp`, `codesign.cpp`,
  `security_identity.cpp`): writes signatures as `codesign` does, without OpenSSL; Security.framework makes the RSA
  signature. The certificate lives in the iOS Keychain (ADR-0022, ADR-0023).

### 4.3 Loader (`runtime/src/loader.cpp`, `linker.cpp`, `dl.cpp`)

- **A linker with Android's semantics,** written for Minato: load groups, eager binding, symbol versions, GNU and
  SysV hash, RELA, RELR and APS2 relocations, IRELATIVE, TLS (initial-exec and TLSDESC), `init_array` order, and
  `dlopen`, `dlsym`, `dladdr` and `dl_iterate_phdr` for unwinders and C++ exceptions. Nothing is ever unloaded.
- **Containers:** the loader opens a container with the host's `dlopen`, which maps and checks the signed code, and
  then relocates the guest image itself; signed code is never written. Libraries with text relocations are
  incompatible.
- **Namespaces** as bionic's (ADR-0040): `default`, the isolated `com_android_art`, and app namespaces created
  through `ld-android.so`'s `android_create_namespace`, linked to `default` by `public.libraries.txt`.
- **Search order** as `linker64`'s: `LD_LIBRARY_PATH` (in `default` only), then the namespace's paths;
  `/system/lib64` holds the guest system's containers from the app bundle.
- **Overrides:** a per-app symbol override table, empty unless an app's profile asks for it (ADR-0066).

### 4.4 Rewriting at import (`importer/converter/src/rewrite.cpp`, ADR-0010)

- **`mrs xN, TPIDR_EL0`** (practically every library: the NDK's stack protector, ELF TLS) becomes a branch into an
  island that loads the guest thread pointer from Darwin TSD slot 6 and branches back; it costs 1.9 cycles per read
  on the iPhone.
- **`msr TPIDR_EL0, xN`** (libc and language runtimes) becomes an island that writes the same slot.
- **`svc #0`** (rare: Go, statically linked libc, raw syscall wrappers) becomes a branch into a veneer that saves
  the registers, calls the syscall layer and keeps Linux's error convention.
- **`x18` as a register** (clang 8 or older, such as Unity up to 2020.3 and old SDK binaries): each use runs in an
  island that keeps the value in TSD slot 11, 1 to 4 cycles more per pass (ADR-0020). Forms without an island make
  the library incompatible.
- **Text relocations** make a library incompatible.

Island areas lie before and after the code, because a branch reaches only 128 MB; a library with more than about
256 MB of code is incompatible for now. Only unambiguous patterns in ranges that `.eh_frame` marks as functions are
replaced, and every patch is counted. There is no trapping and no patching at run time. Libraries laid out for 4 KB
pages are shifted as a whole inside their container (ADR-0007).

### 4.5 Guest libc and the Linux personality (`runtime/bionic/`, `runtime/src/`, ADR-0009)

**Guest libc:** AOSP Bionic at `android-15.0.0_r1`, built with the NDK by `tools/build-bionic.sh` from Bionic's own
build files. The overlay is small: `setjmp.S` without ShadowCallStack and `x18`; start code that sets up the main
thread as `linker64` does; registration of TLS modules; a signal restorer in place of the vDSO's; Minato's own
`ld-android.so`, `liblog.so` and `libnetd_client.so`; and Scudo's configuration with 64 MiB regions (ADR-0035).

**NDK libraries** (`runtime/android/`, `graphics/`): the NDK's API 35 set, among them `libandroid`,
`libnativewindow`, `libaaudio`, `libOpenSLES`, `libmediandk`, `libcamera2ndk`, `libjnigraphics`, and the EGL, GLES
and Vulkan front libraries. Where a library is only needed so that an app loads, `runtime/android/stubgen.py`
generates stubs.

**The syscall layer** is a small kernel in user space; Linux syscall numbers never reach Darwin.

- *Memory* (`sys_memory.cpp`): Minato's own address space manager for `mmap`, `munmap`, `mprotect`, `mremap` and
  `madvise`. It never hands out executable memory.
- *Files* (`vfs.cpp`, `sys_files.cpp`, `descriptors.cpp`): a VFS with mounts (table below) and Linux's flags,
  structures and errors; every path resolves beneath its mount (ADR-0052). The guest's descriptor table has
  Linux's numbering and `RLIMIT_NOFILE` 32,768, with `eventfd`, `epoll`, `ppoll`, `pselect6` and `memfd`
  (ADR-0027).
- *Threads* (`threads.cpp`, `sys_threads.cpp`, `sys_futex.cpp`): `clone` creates a Darwin pthread that starts in the
  parent's register state. Blocking syscalls wait on the thread's signal word, so a signal always ends them; futexes
  keep a wait queue of their own (ADR-0026).
- *Signals* (`signals.cpp`, `sys_signals.cpp`): dispositions, masks, pending and queued signals and alternate stacks
  are emulated, and delivery uses a Linux `rt_sigframe`. Host signals serve only hardware faults, nudging threads and
  default-action exits. A thread switches exactly between guest and runtime code (ADR-0025).
- *Time:* the clock mapping of F10.
- *Network* (`sys_sockets.cpp`, `dns_proxy.cpp`, `netlink.cpp`): sockets over non-blocking BSD sockets with Linux's
  structures and options; a local DNS proxy that reads the guest's `/etc/hosts` first; netlink interface and route
  dumps; `EPERM` sockets for an app without the INTERNET permission (ADR-0054).
- *System* (`proc.cpp`, `properties.cpp`, `hwcap.cpp`): `/proc/self/{maps,smaps,cmdline,exe,auxv}` and
  `/proc/cpuinfo`; `AT_PAGESZ` 16384; `AT_HWCAP` from the device (ADR-0047); the property area that
  `android.os.Build` reads, with an optional device profile (ADR-0065).
- *Private syscalls* (`runtime/include/APKRuntime/guest_interface.h`): numbers above Linux's range for the loader,
  the host tables, window gates, channels, audio, media, sensors, location and permissions.

| Android path | On the iPhone |
|---|---|
| `/data/data/<pkg>`, also as `/data/user/0/<pkg>` | `Library/Application Support/data/<pkg>/data` |
| `/data/user_de/0/<pkg>` | `…/data/<pkg>/data_de` |
| `/storage/emulated/0/Android/data/<pkg>`, also under `/sdcard` | `…/data/<pkg>/external` |
| `/storage/emulated/0/Android/obb/<pkg>` | `…/data/<pkg>/obb` |
| `/storage/emulated/0/Download` | `…/data/<pkg>/download` |
| `/data/app/~~<a>/<pkg>-<b>/base.apk` and its splits, read-only | the installed version under `…/apps/<pkg>/` |
| `/system`, `/apex`, `/system/lib64`, read-only | the guest system in the app bundle (`Guest/`) |
| `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom` | the host's devices |

### 4.6 ABI boundary between guest and host (ADR-0011, ADR-0030)

- Guest code reaches the host only through syscall veneers or the front libraries' imports.
- Every front-library call into host code goes through the runtime's entry `apkrt_host_call`, so no guest signal is
  delivered inside ANGLE, MoltenVK or the graphics host, and a fault inside host code counts as the runtime's.
- `tools/frontgen/` generates the stubs from `vk.xml`, `gl.xml` and `egl.xml`: plain, with small integers extended,
  or with stack arguments repacked. Varargs, `va_list` and `long double` never cross.
- Calls from host to guest (audio, Choreographer, input) are posted into channels that the guest's own threads read;
  host code never runs guest code on a host thread.
- Host errors become Linux errno values; times follow F10.

### 4.7 Graphics (`graphics/`, ADR-0005, ADR-0013, ADR-0029, ADR-0030, ADR-0044, ADR-0058)

- **Windows:** an `ANativeWindow` is a view whose layer is `MinatoMetalLayer` (`graphics/host/layer.m`), a
  `CAMetalLayer` that the benchmark can record. The runtime's window registry (`runtime/src/windows.cpp`) holds each
  window's gate, which stops GPU work while the app may not draw, and the buffers for drawing by the CPU.
- **EGL and GLES:** the guest front libraries in `graphics/egl` and `graphics/gles` call Minato's own ANGLE build
  (branch `chromium/8037`), embedded as `ANGLE.xcframework` and opened with `dlopen`. Metal is the default backend
  (ES up to 3.0); an app that requires ES 3.1 gets ANGLE's Vulkan backend over Minato's MoltenVK (ADR-0044). The
  patches in `third_party/angle/patches` add, among others, a fixed window size, the commit gate,
  `eglPresentationTimeANDROID`, RGB configurations, an iOS display for the Vulkan backend and external images.
- **Vulkan:** the guest `libvulkan` (`graphics/vulkan`) answers as Android 15's loader does and drives MoltenVK
  1.4.2, built from source with Apple's private Metal API compiled in but enabled for ANGLE's instance only. It maps
  `VK_KHR_android_surface` to a Metal surface, copies RGBA8 swapchain images to BGRA8, and imports
  `AHardwareBuffer`s. There is no Vulkan in the Simulator.
- **Host side:** `graphics/host` provides the host tables of the front libraries, the layers, hardware buffers as
  IOSurfaces (ADR-0058), video decoding through VideoToolbox and media playback through AVPlayer (ADR-0060).
- **Views:** hwui and Skia run as guest code with Skia's CPU backend (`graphics/hwui`, ADR-0043), so views draw in
  software.
- **Background:** iOS forbids GPU work in the background. The gate holds every ANGLE commit and every Vulkan submit,
  present and acquire before the app is told that it is paused.

### 4.8 Frame pacing, input and audio (ADR-0036, ADR-0068)

The host reaches the guest through channels: guest descriptors whose pipe holds a byte per arrival, so that
`ALooper` and Java's `MessageQueue` wake on them. The records themselves live in the runtime, behind private
syscalls (`runtime/src/channels.cpp`).

- **Frames:** `DisplayLinkClock` (`app/Sources/Session/FrameClock.swift`) runs CADisplayLink on its own thread while
  the gate is open and posts each tick into the vsync channel; `AChoreographer` and Java's `Choreographer` run their
  callbacks from it. `ANativeWindow_setFrameRate` sets the preferred rate, 60 or 120 Hz.
- **Touch:** `TouchTranslator` turns `UITouch` events, coalesced ones included, into motion events with history and
  Android's pointer IDs; `AInputQueue` and Java's `InputEventReceiver` read them.
- **Keys and text:** each channel record has a kind: motion, key, text or device. Hardware keyboard keys become key
  events; the soft keyboard is a hidden UIKit text input whose commits, deletions and editor actions reach the
  focused view's `InputConnection`. Game controllers reach apps as Android 15's standard pad (ADR-0068).
- **Audio:** Minato's `libaaudio` keeps each output stream's ring of float32 frames in runtime memory
  (`runtime/src/audio.cpp`), and `AVAudioEngine` plays every stream through a source node that reads the ring
  without a lock. `libOpenSLES` and Java's `AudioTrack` run on `libaaudio` streams. Recording comes through
  `AudioRecord`, AAudio input streams and OpenSL ES recorders from the iPhone's microphone (ADR-0064).

### 4.9 ART and the framework (`runtime/art/`, `framework/`, ADR-0033, ADR-0039, ADR-0045, ADR-0053)

- **ART as guest code:** built from AOSP `android-15.0.0_r1` with the NDK (`tools/build-art.sh`, patches in
  `runtime/art/patches`) and loaded by Minato's loader like any guest library, so that JNI with its varargs stays in
  one ABI and ART sees the same Linux personality as the rest.
- **The heap window:** 4 GiB aligned to 4 GiB (2 GiB without `increased-memory-limit`), its first page never mapped;
  32-bit references are offsets from the window's base (F6). Apps run with the CMS collector.
- **Execution:** apps start without a boot image in nterp, ART's fast interpreter. After an app's first close Minato
  compiles it once with its own dex2oat; the code decodes references against the window's base in `x20` and goes
  through the same convert, sign and `dlopen` path as native libraries (ADR-0053).
- **Framework:** `framework.jar` is compiled from a bounded list of AOSP sources (`framework/sources.txt`), Minato's
  own classes under AOSP's names (`framework/java`, among them `ActivityThread`, `PhoneWindow`, `ViewRootImpl` and
  `ServiceManager`), generated managers (`framework/managers`) and patches to AOSP files (`framework/patches`).
  Compile-only stubs (`framework/stubs`) never ship. `framework-res` is built from AOSP's `core/res` with a host
  aapt2 and matches Android 15's resource table byte for byte.
- **Natives:** `libandroid_runtime.so` (`framework/native`) holds the JNI of the framework classes Minato runs,
  `libandroidfw` reads resources, and `app_process64` (`framework/app_process`) starts the VM.
- **Services:** there are no Binder processes. System services are local Binder objects in one `ServiceManager`
  table, behind AOSP's managers or generated stubs that answer Android's defaults and log their first use.
- **Cryptography:** Conscrypt with BoringSSL, BouncyCastle, okhttp and Android's CA certificates (ADR-0046).
- **Host tools,** built from AOSP sources into `build/` and never shipped: aapt2, aidl, and generators for feature
  flags, app ops and compat changes.
- **License:** libcore is GPL-2.0 with the Classpath Exception and ships as a separate component (ADR-0017).

### 4.10 Process model, sessions and windows (ADR-0003, ADR-0032, ADR-0038, ADR-0041)

- **Restart:** the library writes a launch request and asks iOS to open Minato again: first through
  `LSApplicationWorkspace` with this install's bundle ID (private API, kept in `RestartWorkspace.m`), then through
  `minato://open`, each with 3 s for an answer; otherwise an alert asks the user to open Minato again. The new
  process reads the request once, so a crash never leads back into the app.
- **Why a restart:** only one app runs per process, because ART and loaded libraries cannot be unloaded.
- **Session:** the session maps iOS's events (foreground and background, the lock, interruptions, memory warnings,
  thermal state, battery) to Android's lifecycle and broadcasts, and ends the process when the app finishes.
- **Windows:** each Android window (an activity, a dialog, a toast, a popup, a `SurfaceView` layer) is a layer in the
  session's tree, in Android's z order, with focus and touches on the top one (`APKSession/WindowOrder.swift`).
- **Orientation:** the library is portrait only. A session takes the app's `screenOrientation` and turns while it
  runs if the activity declares `configChanges` for it (ADR-0038).
- **Menu:** a swipe down with three fingers opens Minato's menu with Quit and the FPS display; Android's back gesture
  reaches the app.

### 4.11 System integration

- **Files and pickers** (ADR-0057): document and photo intents open iOS's pickers; picked files become clones in the
  app's storage behind content URIs, with changes written back; saving to Photos asks once.
- **Intents to iOS** (ADR-0062): web links open in a Safari view inside Minato; mail, messages, calls, maps and shares
  go to iOS.
- **Sensors** (ADR-0061): CoreMotion feeds the runtime's sensor hub (`runtime/src/sensor_hub.cpp`), which
  `ASensorManager` and Java's `SensorManager` read.
- **Location** (ADR-0063): CoreLocation feeds a location hub (`runtime/src/location_hub.cpp`); iOS's prompt decides
  the permission.
- **Media** (ADR-0060): video `MediaCodec` on VideoToolbox, `MediaExtractor` on Minato's MP4 reader
  (`runtime/android/mp4_extractor.cpp`), `MediaPlayer` on AVPlayer.
- **Per-app profiles** (ADR-0065, ADR-0066): an optional device profile and an optional FPS unlock, stored with the
  app's data.

## 5. Reference projects

- **ARTBox** (MIT): its design notes and its list of Bionic sources for the tag were read. No code is taken; it
  never ran on an iPhone and targets only self-built ELF files.
- **LiveContainer** (AGPL-3.0): concepts only, namely the certificate import, signing every Mach-O with one team,
  `dlopen` from the data container and the restart in place. No code, and no app extensions.
- **Android Translation Layer** (GPL-3.0): concepts only, namely the cut between the app and the Java framework, a
  NativeActivity host and path virtualization. No code.
- **AOSP** (Apache-2.0 and BSD; libcore GPL-2.0 with the Classpath Exception): Bionic, ART, libcore, ICU,
  Conscrypt, frameworks/base, hwui and Skia are built from pinned sources; everything else is matched against
  Android 15's behavior.
- **ANGLE** (BSD-3-Clause) and **MoltenVK** (Apache-2.0): built from pinned sources with small patches.
- **StikJIT and StikDebug** (MPL-2.0, AGPL-3.0): a possible optional JIT path later (ADR-0002).
- **bundletool** (Apache-2.0): a Mac tool for `.aab` files and a reference for `.apks`.
- **KosmicKrisp** (MIT): watched as a Vulkan option; no iOS support yet.

Everything adopted is listed with its version and license in [THIRD_PARTY.md](../THIRD_PARTY.md).

## 6. Dependencies

- **Apple:** Xcode 27 with the iOS 27 SDK; XcodeGen (MIT) generates the project.
- **Build tools:** CMake (BSD-3-Clause), Ninja (Apache-2.0), Android NDK r30 (Apache-2.0 and others), Bison 3 for
  the host aidl.
- **AOSP sources** at `android-15.0.0_r1`, pinned by digest in `third_party/sources.json`: Bionic, ART, libcore,
  ICU, Conscrypt and BoringSSL, the framework, hwui, Skia and their libraries.
- **Graphics:** ANGLE (`chromium/8037`), MoltenVK 1.4.2 with SPIRV-Cross, SPIRV-Tools and cereal.
- **Build-only:** JDK 17 (Temurin, GPL-2.0 with the Classpath Exception), R8 and D8 (BSD-3-Clause), bundletool 1.18.3
  (Apache-2.0), and Godot 4.7.2 (MIT) for one benchmark's test app.
- **Reference only:** the Android emulator with an Android 15 arm64 system image (ADR-0014).

## 7. Risks

- **R1.** Thread pointer and TLS in all prebuilt libraries (F3). *Handled* by islands at import (ADR-0010).
- **R2.** Converting, signing and loading Android code on the iPhone. *Handled* since M2, on the device with the
  imported certificate.
- **R3.** Memory with a free account (F12, F13). *Medium.* `increased-memory-limit` where the installer grants it,
  and the budgets of ADR-0012 and ADR-0035.
- **R4.** ART's heap above 4 GiB (F6), and the interpreter's speed. *Medium.* The heap window is done; nterp is the
  default and apps are compiled after their first close (ADR-0053).
- **R5.** OpenGL ES 3.1 and later (F14). *Medium.* ES 3.1 runs on ANGLE's Vulkan backend, slower under heavy load;
  ES 3.2 is not offered.
- **R6.** No Android device. *Low.* Speed is compared with the native iOS version, output with the emulator.
- **R7.** Errors at the ABI boundary (F8). *Low.* Generated thunks; tests with garbage in the upper register bits.
- **R8.** Libraries aligned to 4 KB (F7). *Low.* Shifted as a whole (ADR-0007); layouts with two boundaries stay
  refused.
- **R9.** Crash handlers that expect a Linux `ucontext`. *Low.* Delivery with a Linux `rt_sigframe`.
- **R10.** Private APIs and dyld internals change. *Medium.* Minato writes its own Mach-O files, and private API
  sits only in `RestartWorkspace.m`, with a public fallback.
- **R11.** Third-party code of uncertain quality. *Low.* No ARTBox code is taken, and every source is pinned by
  digest.
- **R12.** Certificates from Xcode and SideStore collide or get revoked. *Medium.* The app follows the certificate of
  its own signature and signs everything again (ADR-0023).
- **R13.** Overhead of islands, syscalls, the swapchain copy and the interpreter. *Medium.* Measured: 1 to 4 cycles
  per island, about 21 ns per syscall; the parity results are in [BENCHMARKS.md](BENCHMARKS.md).
- **R14.** Xcode's new project format and XcodeGen. *Low.* Stay on Xcode 27.0's format until XcodeGen supports it.
- **R15.** The Simulator misleads: another signature check, no Jetsam, no Vulkan, ANGLE at ES 3.0. *Low.* The
  Simulator serves UI and harness checks; the loader is tested on the Mac and the iPhone.
- **R16.** GPL and AGPL references, and libcore. *Low.* Concepts only; libcore is a separate component (ADR-0017).
- **R17.** `x18` in builds with clang 8 or older (F4). *Medium.* Islands (ADR-0020); some forms are still refused,
  among them in the Unity test APK.

## 8. Open questions

- The start time and memory cost of compiled Java code (ADR-0053).
- The remaining `x18` forms in old Unity builds (ADR-0020).
- NativeActivity apps on the Java path, after a parity rerun (ADR-0069).
- Camera2 and the NDK camera (ADR-0073).
- MetalFX upscaling (the draft ADR-0067).
- KosmicKrisp in place of MoltenVK once it supports iOS.
- An optional JIT path through StikDebug for apps that need one (ADR-0002).

## 9. Main sources

- **ARTBox:** github.com/j0shua-SYSON/ARTBox (`docs/loader-design.md`)
- **LiveContainer:** github.com/LiveContainer/LiveContainer
- **SideStore** (read only): github.com/SideStore/SideStore
- **Android Translation Layer:** gitlab.com/android_translation_layer (`doc/Architecture.md`)
- **XNU:** github.com/apple-oss-distributions/xnu (`pcb.c`, `locore.s`, `sleh.c`, `mach_loader.c`,
  `ux_exception.c`)
- **Apple:** "Writing ARM64 code for Apple platforms", "Supported capabilities (iOS)", "Compare memberships", the
  iOS 27 and Xcode 27 release notes
- **LLVM:** `AArch64ISelLowering.cpp`, `AArch64TargetParser.cpp`
- **Android:** "Support 16 KB page sizes", the NDK release notes, AOSP `bionic/linker/linker_phdr_16kib_compat.cpp`
- **Graphics:** ANGLE (`DisplayMtl.mm`, `SurfaceMtl.mm`), MoltenVK (`MVKDevice.mm`), Mesa's KosmicKrisp documentation
