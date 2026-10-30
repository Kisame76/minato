# Design decisions

Minato records its architecture decisions as numbered records (ADRs). Code comments, commit messages and the other
documents refer to them as "ADR-00NN", often with a decision or step inside it, such as "ADR-0045 decision 6" or
"ADR-0058 b)". The full records, with the context, the options weighed, the tests and the measurements, are kept as
an internal working log. This page is the public index: for each ADR its title, its status today and the decision
as it stands now.

Status values:

- **Accepted:** in force. "Amended" means that a later change replaced a named part; the text below gives the
  current form.
- **Proposed:** written down and followed in practice, but never formally accepted.
- **Draft:** under discussion, not in force.

No ADR has been rejected or withdrawn as a whole. Options that were rejected are named inside the ADR that rejected
them. Where a later ADR replaces part of an earlier one, both entries below say so. Facts F1 to F18 and risks R1 to
R17 are those of [ARCHITECTURE.md](ARCHITECTURE.md). Status as of 2026-10-30.

## By topic

| Topic | ADRs |
|---|---|
| Fixed principles | 0001 to 0007 |
| Loader, containers and machine code | 0008, 0010, 0011, 0019, 0020, 0021, 0025, 0030, 0040 |
| Guest libc and the Linux syscall layer | 0009, 0012, 0026, 0027, 0035, 0047, 0052, 0054 |
| Signing | 0022, 0023 |
| Toolchain, account, licenses, name | 0014, 0015, 0016, 0017, 0024 |
| Import and formats | 0018, 0031, 0034, 0048, 0049, 0050, 0051, 0055 |
| Sessions, windows and the app | 0003, 0032, 0038, 0065, 0066 |
| Graphics | 0005, 0013, 0029, 0030, 0043, 0044, 0058, 0067 |
| Frame pacing, input and audio | 0036, 0068 |
| ART and the framework | 0004, 0033, 0039, 0041, 0045, 0046, 0053, 0059, 0069 |
| Device features and system integration | 0056, 0057, 0060, 0061, 0062, 0063, 0064 |
| Measurement | 0014, 0037, 0042 |
| Releases | 0070 |

## The records

### ADR-0001: No emulation

Accepted. No CPU emulator, no guest kernel and no virtual machine: Android arm64 code runs directly on the iPhone's
CPU, and only the Android environment is recreated. Replacing a few single instructions before signing (ADR-0010)
and a Linux syscall layer in user space (ADR-0009) count as adaptation, not as emulation.

### ADR-0002: No JIT in the core path

Accepted. Everything that runs exists as a signed file before it runs: the guest system libraries are signed at
build time, an app's native libraries (and later its compiled Java code) at import on the device. Islands, veneers
and thunks are made at build or import time, never at run time (F1). An optional JIT path through StikDebug may come
later for apps with a JIT of their own; it is never a prerequisite.

### ADR-0003: One app per process, restart into game mode

Accepted; the mechanism is specified by ADR-0032. ART allows one runtime per process and loaded libraries cannot be
unloaded, so each Android app runs in a process of its own: the library writes a launch request and restarts
Minato, which boots straight into the app ("game mode" in these documents; the app's UI never says "game").
Quitting restarts into the library. Running apps in app extensions was rejected: smaller memory limits, aggressive
termination, and an App ID per extension.

### ADR-0004: Framework in-process

Accepted. Android's APIs are recreated inside the app's process, without Binder IPC and without system services as
separate processes; services are in-process objects (ADR-0045). App DEX code runs on real ART, and the cut lies
between the app and the Java framework.

### ADR-0005: Graphics stack

Accepted; the GLES part is refined by ADR-0013 and ADR-0044. EGL and OpenGL ES go to ANGLE with its Metal backend,
Vulkan goes to MoltenVK, `VK_KHR_android_surface` becomes `VK_EXT_metal_surface`, and AChoreographer is driven by
CADisplayLink at up to 120 Hz. KosmicKrisp is watched as a later Vulkan option once it supports iOS.

### ADR-0006: arm64-v8a only

Accepted. Only arm64-v8a code runs. An APK with only 32-bit code is marked "Incompatible" at import, since iPhones
cannot run AArch32 since the A11; x86 and x86_64 libraries are ignored, and a fat APK uses its arm64-v8a libraries.

### ADR-0007: 16 KB pages

Accepted; decision 2 amended. iOS uses 16 KB pages. Libraries aligned for 16 KB load as they are. Libraries laid out
for 4 KB pages load too: the converter moves the whole ELF image by 0, 4, 8 or 12 KB inside its container, as
AOSP's 16 KB compatibility mode moves its load bias, so that the one boundary between code and data starts a 16 KB
page; no instruction or table is rewritten. A layout with a second boundary is refused as "Incompatible: 4 KB
layout". The guest sees `AT_PAGESZ` 16384.

### ADR-0008: Load guest code as signed Mach-O dylibs

Accepted; amended (one Mach-O segment per protection run); its signing plan is replaced by ADR-0022. Every ELF
library or program is converted into a "container": a Mach-O dylib that keeps the ELF segments at their distances,
has island areas around the code and no chained fixups (Minato's loader relocates), and is signed with the app's
bundle ID as identifier, so that `dlopen` accepts it. Library Validation is not bypassed: all code carries the same
team's signature. The original ELF stays next to its container, so that everything can be signed again when the
certificate changes.

### ADR-0009: Guest libc: real AOSP Bionic plus a Linux syscall layer

Accepted. The guest uses AOSP's Bionic (`android-15.0.0_r1`) with a small overlay, and the runtime implements the
Linux syscall interface in user space: memory, files, threads, futexes, signals, time, `/proc`. Errno values,
flags, structures and signal frames are translated at this one narrow point instead of in hundreds of libc
functions (F8, F9).

### ADR-0010: Adapt machine code at import

Accepted; its `x18` part is updated by ADR-0020. Before signing, the converter disassembles every executable range
and replaces exactly these patterns with branches into islands: reads and writes of `TPIDR_EL0` (the guest thread
pointer lives in a Darwin TSD slot, F3), `svc #0` (into the syscall layer, F5) and uses of `x18`. Only unambiguous
patterns are replaced; there is no trapping and no patching at run time. Code that cannot be rewritten safely makes
its library incompatible.

### ADR-0011: ABI boundary between guest and host

Proposed; its rules are in force as refined by ADR-0030. Guest code reaches host code only through syscall veneers
and the NDK front libraries. A call binds directly only where both ABIs agree; otherwise a generated thunk extends
small integers and repacks stack arguments. Varargs, `va_list` and `long double` never cross the boundary, host
threads that call guest code register as guest threads first, and host errors become Linux errno values.

### ADR-0012: Memory and address space budget

Accepted. Every build requests the `increased-memory-limit` entitlement. The guest budget is at most 5 GB with it and
2.5 GB without it, which is what a free account gets through SideStore (measured). `extended-virtual-addressing` is
not requested. Allocators reserve address space against a central budget (ADR-0035).

### ADR-0013: GLES path

Accepted. ANGLE on Metal serves OpenGL ES up to 3.0. Vulkan through MoltenVK is the main path for modern engines.
ES 3.1 runs on ANGLE's Vulkan backend (ADR-0044). ES 3.2, and Vulkan apps that need geometry shaders, count as
incompatible for now. The import reads each app's `glEsVersion` and Vulkan level.

### ADR-0014: Reference without an Android device

Accepted. Output and behavior are compared with Android 15 in the arm64 emulator, whose logs are the expected
results in `fixtures/expected/`. Speed is compared with the native iOS version of the same app on the same iPhone.
No Android device and no cloud devices are used, and the emulator's frame times are not a reference.

### ADR-0015: Free Apple account

Accepted. The project assumes a free Apple account: profiles valid for 7 days, 3 sideloaded apps per device, 10 App
IDs per 7 days, no extended virtual addressing. Hence one bundle ID and no app extensions.

### ADR-0016: Build system, toolchain, target platform

Accepted; its bundle ID was replaced by ADR-0024. Xcode 27 with the iOS 27 SDK and a deployment target of iOS 26.0.
The Xcode project is generated by XcodeGen from `project.yml` and not versioned. Swift packages use SwiftPM; C and
C++ use CMake presets and Ninja and are packed into XCFrameworks; guest code is built with Android NDK r30, the
fixtures for API level 29. Deployment uses `xcodebuild` and `devicectl`.

### ADR-0017: Reference code and licenses

Accepted. The project license is Apache-2.0. GPL and AGPL projects are read for their concepts only, and their code
never enters the repository. ADR-0033 adds one exception: ART's class library (libcore, GPL-2.0 with the Classpath
Exception), which is fetched at build time and ships as a separate component with its license.

### ADR-0018: Import without a folder in the Files app

Accepted; extended by ADR-0048 and ADR-0049. Minato has no folder in the Files app. Apps come in through "+" (the
document picker, several files at once) or "Open with"; the original stays where it is. Minato installs from a
hidden clone, deletes the clone afterwards, and finishes an interrupted import at the next launch.

### ADR-0019: Load static guest programs

Accepted. The guest thread pointer lives in Darwin TSD slot 6, which Apple leaves free for code with fixed offsets
(slot 11 later holds `x18`). A guest program runs on a thread of its own with an 8 MiB stack and a Linux start
stack, and `exit_group` ends the whole process with the guest's status, as on Android. A container carries
`LC_UUID`, a libSystem dependency and room for its signature (F18).

### ADR-0020: `x18` as a normal register in app code

Accepted; implemented in steps, some forms still refused. XNU zeroes `x18` (F4), but code built with clang 8 or
older (Unity up to 2020.3, old SDK binaries) uses it as a normal register. Every instruction that reads or writes
`x18` becomes an island that keeps the value in TSD slot 11. Forms without an island, such as `ret x18`,
pointer-authenticated branches through `x18` and some load-exclusive windows, still make a library incompatible.

### ADR-0021: Dynamic guest programs

Accepted. Minato's loader plays Android's `linker64`: it links programs and libraries with Android's search order,
symbol versions and TLS (TLSDESC included) and supports `dlopen` at run time. Bionic is built from the pinned AOSP
sources, and the syscall layer provides a guest file system, threads and emulated signals.

### ADR-0022: Signing on the iPhone with our own signer

Accepted; replaces the zsign and OpenSSL plan of ADR-0008. A signer written for Minato in C++, without third-party
code, writes what `codesign` writes: a SHA-256 CodeDirectory with 16 KiB pages, the designated requirement and a CMS
blob with the certificate chain; Security.framework makes the RSA signature. During development,
`tools/deploy.sh` hands the certificate Xcode signs the app with to the app on the device.

### ADR-0023: Signing certificate from SideStore or AltStore

Accepted. The certificate Minato signs converted code with must be exactly the one Minato itself is signed with. At
every launch and every return to the foreground the app compares them and fetches a missing or changed certificate
itself: silently from SideStore's `ALTCertificate.p12` in the bundle, otherwise through a one-time export in
SideStore or AltStore. All converted code is signed again after a change.

### ADR-0024: Name Minato

Accepted. Project, app and repository are named Minato: Japanese for "harbor", in Old Japanese "gate of the water".
The URL scheme is `minato://`. Internal module names (APKKit, APKRuntime, APKConvert and others) keep their names,
since they describe APKs, not the product.

### ADR-0025: Exact switches between guest and runtime code

Accepted. A per-thread flag says whether a thread runs guest or runtime code, and it changes exactly where the code
changes sides; where that cannot be one instruction, the signal handler knows the few instructions in between by
their address. So a guest signal is never delivered inside runtime code, and every fault is attributed to the
right side. The loader's functions became private syscalls for the same reason.

### ADR-0026: Blocking syscalls wait on the thread's signal word

Accepted. A blocking syscall waits on a per-thread word with `os_sync_wait_on_address`, so that a signal always ends
the wait, as on Linux. Futexes keep their own wait queue in the runtime.

### ADR-0027: Guest descriptor table and waits on descriptors

Accepted. The guest has its own descriptor table with Linux's numbering, open file descriptions and
`RLIMIT_NOFILE` 32,768, independent of the host's descriptors. `eventfd`, `epoll`, `ppoll` and `pselect6` wait in
the thread's kqueue, which a signal always ends.

### ADR-0028: The NativeActivity host

Accepted; the library's start path moves to ADR-0069. `activity-host`, a guest program, plays Android's app process
for apps without Java: it opens the app's native library, calls `ANativeActivity_onCreate` and then each callback
in Android 15's order as the host posts lifecycle events through the session. It comes with Minato's `libandroid`,
the window registry with its gate, and a JNI stub.

### ADR-0029: GLES through ANGLE-Metal

Accepted. Minato builds ANGLE itself (branch `chromium/8037`, the Metal backend, a small set of patches) and embeds
its `libGLESv2` as a framework. The guest front libraries `libEGL` and `libGLESv1_CM`, `libGLESv2`, `libGLESv3`
forward to it. A gate holds GPU work while the app may not draw, for example in the background.

### ADR-0030: Calls from guest into host code; Vulkan through MoltenVK

Accepted. Every call of a front library into host code goes through one runtime entry, `apkrt_host_call`, so no
guest signal is delivered inside ANGLE or MoltenVK. A generator (`tools/frontgen/`) builds the stubs from the
Khronos registries. Minato's `libvulkan` answers as Android 15's loader does (Vulkan 1.3, Android's extension
lists), maps the Android surface to a Metal surface and copies RGBA8 swapchain images to BGRA8 at present.

### ADR-0031: make-apk, a packer for our test apps

Accepted. `make-apk` (in `importer/APKKit`, run through `tools/make-apk.sh`) compiles a manifest, lays out an APK
with native libraries, DEX files and assets, and signs it with signature scheme v2 and a local debug key.

### ADR-0032: Game mode: restart, full-screen window, lifecycle and overlay

Accepted; its decision 5 (orientation) is changed by ADR-0038. A launch request is read once, so a crash never
leads back into the app. The restart tries `LSApplicationWorkspace` with this install's bundle ID, then
`minato://open`, each with 3 s for an answer, and otherwise asks the user to open Minato again. The app runs full
screen with Android's lifecycle order, and a swipe down from the top edge opens Minato's menu (Quit, FPS display).

### ADR-0033: ART on our loader

Accepted; amended. ART from AOSP `android-15.0.0_r1` is built with the NDK as guest code and loaded by Minato's
loader. Because iOS maps nothing below 4 GiB (F6), the Java heap lives in a 4 GiB window (2 GiB without
`increased-memory-limit`) and 32-bit references are offsets from its base. Apps start without a boot image, in the
interpreter, with the CMS collector; compiled code is ADR-0053.

### ADR-0034: Updates by importing a newer version

Accepted. A file with the same signer and a higher `versionCode` updates the app in place and keeps its data; the
same version with other bytes installs again; a lower version or another signer asks first, with a warning; an
unusable new file leaves the installed version as it is.

### ADR-0035: Scudo's address space and RLIMIT_STACK

Accepted. Bionic's Scudo allocator uses 64 MiB regions that a size class reserves on first use, at most about 2 GiB
of address space instead of a large reservation up front. `RLIMIT_STACK` answers as on Android: 8 MiB soft, no hard
limit.

### ADR-0036: Frame pacing, touch and audio

Accepted. The host reaches the guest through channels: guest descriptors that an `ALooper` can wait on, whose
records live in runtime memory behind private syscalls. A `CADisplayLink` on its own thread feeds `AChoreographer`;
`UITouch` becomes motion events with history; Minato's own `libaaudio` and `libOpenSLES` write ring buffers that
`AVAudioEngine` plays without locks.

### ADR-0037: The parity benchmark

Accepted; the rule for undecided verdicts was changed later. One C scene is built twice, as GLES and Vulkan APKs in
Minato and as a native iOS framework, and both are measured alike on the iPhone: median FPS and p99 frame time over
the warm part of five valid runs per variant, in A-B-B-A order. An APK passes with at least 95 % of the native FPS
and at most 105 % of the native p99.

### ADR-0038: The library in portrait, sessions that turn, and iOS Game Mode

Accepted. The library is portrait only. A running app takes its orientation from its `screenOrientation` and turns
while it runs when it declares `configChanges` for it, with Android 15's order of events. Every build declares iOS
Game Mode.

### ADR-0039: The app process and the framework core

Accepted; its NativeActivity part is continued by ADR-0069. `app_process` starts ART and Minato's `ActivityThread`
on the guest main thread. The framework is AOSP's own Java wherever it stands on the device: `framework.jar` is
compiled from a bounded list of AOSP sources, `framework-res` is built from AOSP's `core/res`, `libandroidfw` reads
resources, and the natives live in one `libandroid_runtime.so`. Paths, uid, properties and `Build` are Android 15's.

### ADR-0040: Linker namespaces in our loader

Accepted. The loader implements bionic's linker namespaces, ported from `linker.cpp` rather than invented:
`default` and the isolated `com_android_art` at start, and the app namespaces that libnativeloader creates from
`public.libraries.txt`. So an app's own copy of a library never collides with ART's.

### ADR-0041: Views, surfaces, input and NativeActivity on the Java path

Accepted; decision 2 (one window per activity) is changed by the second-windows step of the compatibility plan.
AOSP's `View` classes run under Minato's window classes with AOSP's names (`PhoneWindow`, `WindowManagerImpl`,
`ViewRootImpl`). A `SurfaceView` gets a layer of its own; GLES from Java uses AOSP's `android.opengl`; input arrives
through an input channel; insets come from iOS's safe area. Dialogs, toasts, popups and translucent activities are
windows of their own in Android's z order.

### ADR-0042: Test 2 with Godot

Accepted. A Godot 4.7.2 project of our own (`fixtures/apps/godot-bench/`), exported as a Vulkan and a GLES APK, runs
in Minato and is measured against Godot's own iOS build on the same iPhone with ADR-0037's rules. The ADR also set up
the `x18` profile of a Unity test APK.

### ADR-0043: Views and Canvas: hwui on Skia's CPU backend

Accepted. AOSP's hwui and Skia are built as guest code in their host flavor, without a GPU backend, so `Bitmap`,
`Canvas`, `Paint`, text and the image codecs work. Views draw in software into the window's buffers, and
`View.isHardwareAccelerated()` answers false. hwui's GPU pipelines are later work.

### ADR-0044: ES 3.1 through ANGLE on Vulkan

Accepted (option A). An app whose manifest requires ES 3.1 runs on ANGLE's Vulkan backend over Minato's own MoltenVK
1.4.2 build, which turns Apple's private Metal API on for ANGLE's instance only, to get the last-vertex convention
for `flat` values. A check at every ES 3.1 session start refuses the path if the device does not behave. Every other
GLES app stays on ANGLE-Metal.

### ADR-0045: System services, broadcasts and runtime permissions

Accepted; amended. AIDL's Java is generated by AOSP's `aidl`, built from source. System services are local Binder
objects in one in-process `ServiceManager` table, behind AOSP's managers or generated stubs that answer Android's
defaults and log what is not written yet. Broadcasts stay inside the process. Runtime permissions are answered at
once without a dialog: normal ones are granted, other dangerous ones denied; location and microphone now go through
iOS's prompt (ADR-0063, ADR-0064). Due jobs and alarms are to run only while the app is in front.

### ADR-0046: Java cryptography

Accepted. Conscrypt with BoringSSL linked statically into `libjavacrypto.so` (without FIPS mode), BouncyCastle
shrunk with R8, okhttp and Android's CA certificates, at the APEX paths Android 15 uses. TLS 1.2 and 1.3 work.

### ADR-0047: The CPU features the guest is told (AT_HWCAP)

Accepted. At process start the runtime reads the device's `FEAT_*` flags through `sysctl` and reports in
`AT_HWCAP` and `AT_HWCAP2` exactly what the device has. A few features (SSBS, ECV, AFP, WFXT) are reported only after
a check at EL0 on the device.

### ADR-0048: Split sets

Accepted. One import is one file, several files or a folder. Split roles come from the manifests, never from file
names; Android's install checks run in Android's order with its messages. Splits are chosen as bundletool's
`extract-apks` chooses them for the iPhone: arm64-v8a, the screen's density, the preferred languages. A lone split
joins an installed version as Android's inherit mode does.

### ADR-0049: Containers: .apks, .xapk, .apkm and OBBs

Accepted. Containers are recognized by content. bundletool's `.apks` is chosen from its table of contents; SAI's
and App Manager's `.apks`, a plain `.zip` and a plain `.apkm` give their APKs, which are chosen as a loose set. An
XAPK's OBB files go to the app's OBB folder; a single `.obb` is matched by its name or by asking. An encrypted
`.apkm` is refused with its reason. Extraction is streamed, with CRC and size checks and fixed caps.

### ADR-0050: Play Asset Delivery and Play Feature Delivery at run time

Accepted. Install-time asset packs are splits. Fast-follow and on-demand packs that the import brings are kept and
laid out for the Play libraries' local-testing path, so the app's own asset-delivery library finds them. Feature
modules come only through the import. Nothing talks to the Play Store.

### ADR-0051: .aab to a universal APK, a Mac tool

Accepted. `tools/aab-to-apk.sh` runs bundletool's universal mode with Minato's JDK and aapt2 and always signs with a
local test key. The app itself does not take `.aab` files.

### ADR-0052: A guest's symlinks stay inside the guest's file system

Accepted; part 1 implemented. Every host call that takes a guest path resolves beneath its mount's host folder
(`O_RESOLVE_BENEATH`, `AT_RESOLVE_BENEATH`), so neither a link nor ".." can reach outside; the guest gets `EACCES`.
Part 2, Linux's own link semantics across mounts, comes when an app needs it.

### ADR-0053: Compiled Java code: nterp and dex2oat with the reference codec

Accepted; in progress. Register `x20`, ART's marking register, which a build without read barriers does not use,
holds the heap window's base; compiled code, nterp and the stubs decode 32-bit references against it, and implicit
null checks stay through the window's unmapped first page. Compiled code comes from Minato's own dex2oat and goes
through the same convert, sign and `dlopen` path as native libraries. nterp, a verify-only dex2oat and compiled
code that decodes references run in tests; apps still start in the switch interpreter.

### ADR-0054: Networking: sockets and DNS

Accepted; partly implemented. Guest sockets are descriptions in the guest's table over non-blocking host sockets,
with Linux's structures, flags and options translated by table. Names resolve through a local DNS proxy that answers
from the guest's hosts file first and then from the iPhone's resolver; netlink answers interface and route dumps; an
app without the INTERNET permission gets `EPERM` sockets. Multicast and broadcast are not offered. The check on the
real network on the iPhone is still open.

### ADR-0055: The import's compatibility report

Accepted; its library half is implemented. The import runs every check over the whole app instead of stopping at
the first problem: each native library through the converter's check mode, its NEEDED names and undefined symbols
against the guest's export table, and the libraries the Java code loads by name. Each finding blocks, degrades or
informs and names its cause; Get Info lists them. The DEX and manifest half and the re-check after a Minato update
are planned steps [their state not verified for this page].

### ADR-0056: WebView on WKWebView

Accepted; not built yet. AOSP's `android.webkit` classes over a provider of Minato's whose WebView is a WKWebView
placed in the session at the view's frame, with Android 15's WebView user agent and app-local pages through a
`minato-asset://` scheme. Today no provider exists, so creating a WebView fails.

### ADR-0057: Files, pickers and shared media

Accepted. Android's document, content and photo-picker intents are answered by iOS's pickers
(`PHPickerViewController`, `UIDocumentPickerViewController`). Picked files become clones in the app's storage under
content URIs, and changes are written back to the original. Saving to Photos asks once.

### ADR-0058: SurfaceTexture and HardwareBuffer on IOSurface

Accepted; decision 1 is changed for decoder output by ADR-0060. An `AHardwareBuffer` is an IOSurface. External
textures work on ANGLE-Metal through ANGLE patch 0010; AOSP's `SurfaceTexture`, `TextureView`, `HardwareBuffer` and
`ImageReader` run over a buffer queue of Minato's; Vulkan imports hardware buffers. In the Simulator, frames are
copied where wrapping an IOSurface fails.

### ADR-0059: The AndroidKeyStore provider

Accepted; only step 0 is implemented. AOSP's keystore2 Java provider over a local keystore service with a software
KeyMint on Conscrypt. Key blobs live in the app's data folder, sealed by a per-app wrapping key in the iOS Keychain;
the security level is SOFTWARE, with no StrongBox and never Google's attestation. So far only the parameter classes
such as `KeyGenParameterSpec` are in `framework.jar`.

### ADR-0060: MediaCodec, MediaExtractor and MediaPlayer on VideoToolbox and AVFoundation

Accepted; in progress. Each video `MediaCodec` is a VideoToolbox decompression session (H.264, then HEVC) behind one
codec core that audio backends will share; frames reach a `Surface` with one GPU copy. `MediaExtractor` is Minato's
own MP4 (ISO BMFF) reader, and `MediaPlayer` plays through `AVPlayer`. Audio codecs come in a later step.

### ADR-0061: Sensors on CoreMotion

Accepted. Accelerometer, gyroscope, magnetic field, gravity, linear acceleration, both rotation vectors and the
uncalibrated forms, with Android's units and axes, mapped from one `CMMotionManager` per process.

### ADR-0062: Intents that leave the app for iOS

Accepted. `http` and `https` links open in a Safari view inside Minato; `mailto`, `sms`, `tel` and map links go to
iOS's composers or are opened by URL; `ACTION_SEND` opens the share sheet. Play Store links behave as on a phone
without the Play Store: web links open in the browser, `market:` has no handler.

### ADR-0063: Location on CoreLocation

Accepted. AOSP's `LocationManager` over a local service, with the providers Android 15's emulator shows (`gps`,
`fused`, `passive`) served from CoreLocation through one location hub in the runtime. The app's permission request
leads to iOS's own prompt; coarse and fine map to iOS's accuracy levels. No background location and no satellite
data.

### ADR-0064: Microphone input on AVAudioSession

Accepted; only the permission path and the audio-port classes are implemented. Recording is to come through
`AudioRecord`, AAudio input streams and OpenSL ES recorders over one input engine on `AVAudioSession`, with
`RECORD_AUDIO` decided by iOS's prompt as for location. Today AAudio input streams still answer
`AAUDIO_ERROR_UNAVAILABLE`.

### ADR-0065: Device profiles

Accepted. A per-app, opt-in profile, never a global one and never a condition for compatibility: a Pixel-class or a
Galaxy-class phone's `android.os.Build` names and GPU name strings in GLES and Vulkan. Extension lists and limits
stay truthful. Without a profile an app sees Minato's own values.

### ADR-0066: FPS unlock

Accepted. A per-app, opt-in frame rate of 60 or 120 Hz above an app's own cap. The frame clock prefers that rate, and
for Unity apps a loader-side wrapper of the engine's `targetFrameRate` setter lifts a lower value. No byte of the
app's libraries changes, and nothing is derived from decompiled code.

### ADR-0067: MetalFX upscaling

Draft, not in force; waiting for measurements. A per-app render scale for native surfaces (SurfaceView and
NativeActivity windows) with MetalFX's spatial upscaler; Java views keep the screen's size, and there is no temporal
upscaler and no frame interpolation.

### ADR-0068: Text input, hardware keys and controllers

Accepted; partly implemented. Records on the input channel carry a kind: motion, key, text or device. Hardware keys
reach windows as key events. The soft keyboard is a bridge in Minato's `InputMethodManager` to a hidden UIKit text
input, whose callbacks drive the focused view's `InputConnection`. Still open: the keyboard's inset (resize and pan)
and game controllers through `GCController` as input devices.

### ADR-0069: NativeActivity apps on the Java path

Accepted; its plan's steps 1 to 3 are implemented, the switch in the library is not. An app whose launcher activity
is `NativeActivity` or a subclass starts through
`app_process` with AOSP's `NativeActivity.java`, so its JNI calls reach a real framework. `activity-host` stays for
the Mac harness, the graphics tests and the parity benchmark; the library moves its start path once the benchmark
has run again on the Java path. Measured on the Mac: about 0.5 s more per warm start and about 98 MB more peak
memory.

### ADR-0070: Public beta releases, changelog and app reports

Accepted. Public releases are betas, `v1.0.0-beta.N`, until `v1.0.0`, which comes when the 22 compatibility gaps
are closed or named as limits. `CHANGELOG.md` follows Keep a Changelog, and every change users notice adds a bullet
under Unreleased. A pushed version tag is the release: a workflow creates the GitHub release from the changelog,
and `tools/release.sh` builds the IPA on a Mac, signed ad hoc for SideStore and AltStore to sign again. Apps that
do not work are reported through an issue form; a reporter's APK never enters the repository. The process is in
[RELEASING.md](RELEASING.md).
