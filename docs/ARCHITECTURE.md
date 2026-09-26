# Minato – Architecture

As of: M0, 2026-09-21; spike results, M2 step 4, the `x18` rule and M2 step 5 added 2026-09-22; `x18` in islands and the costs of the islands added 2026-09-24; GLES path and speed reference decided before M3, 2026-09-24. Decisions are in [`DECISIONS.md`](DECISIONS.md) (ADR-0001 to ADR-0026), progress in [`STATUS.md`](STATUS.md). The raw research reports with all sources are in [`research/`](research/).

Markers: **[verified]** = read in a primary source or measured, **[inferred]** = own conclusion, **[unverified]** = secondary source or user report.

## 1. Summary

- **Path of a `.so` at import:**
  - Android `.so` files run directly on the iPhone CPU.
  - At import, every `.so` is checked and rewritten at a few precisely defined places (thread pointer accesses, Linux syscalls).
  - Then it is packaged in a Mach-O dylib, signed with our own developer certificate and loaded in the game with `dlopen` (ADR-0008, ADR-0010).
- **Two worlds in the game process:**
  - The **guest world** runs with the Android ABI: real AOSP Bionic, the app's `.so` files and our Android system libraries.
  - The **host world** runs with the Darwin ABI: loader, Linux syscall layer, ANGLE, MoltenVK, AudioUnit and UIKit.
  - There are exactly two transitions: syscalls and the NDK system APIs (ADR-0009, ADR-0011).
- **What is verified:** Signing with our own certificate and `dlopen` work on iOS 27 (LiveContainer, JIT-less mode). Our own tests confirm this also for files in the data container if the signature identifier is the app's bundle ID (S1b). With a different identifier, iOS rejects the copy (S1, ADR-0008).
- **What is not verified** is that Android `.so` files run on a real iPhone. So far ARTBox has shown this only on macOS in CI. That is why three small feasibility tests (spikes) still run in M0 (see [`STATUS.md`](STATUS.md)). Since M2 step 4, static NDK programs run on macOS through the converter, container dylib and loader (ADR-0019); the iPhone follows in step 6.
- **Biggest risks:**
  - Thread pointer in practically every `.so`.
  - Memory and address space with a free account.
  - ART heap versus the 4 GiB `__PAGEZERO` (M4).
  - OpenGL ES ≥ 3.1: ANGLE-Metal only supports 3.0.
  - No Android device: speed is only compared with the native iOS version on the same iPhone (ADR-0014).

## 2. Platform facts that determine the design

| # | Fact | Consequence for us | Evidence |
|---|---|---|---|
| F1 | iOS only runs signed code. `dynamic-codesigning` exists only for Apple. On A15+ (TXM, so also A17 Pro), freshly written code only becomes executable if an attached debugger writes the page. | All guest code must be present as a signed file before launch. JIT exists only as an option via StikDebug. | [verified] Apple Platform Security; StikJIT `INTEGRATION.md` |
| F2 | Library Validation: a process may load libraries of the same Team ID. | Converted `.so` files are signed with a certificate of the same (free) Team ID. No bypass is needed. | [verified] Apple "App code signing process"; LiveContainer JIT-less on iOS 27. Our own tests: from the bundle yes; a copy in the data container only with the bundle ID as signature identifier (S1/S1b, ADR-0008) |
| F3 | XNU overwrites `TPIDR_EL0` with CPU and cluster bits on every context switch. The Darwin thread pointer is in `TPIDRRO_EL0`, which is read-only for apps. | Android code reads the thread pointer via `TPIDR_EL0`: the NDK default stack protector at TP+0x28 and ELF TLS from minSdk 29. This does not work unchanged, so it is rewritten at import (ADR-0010). | [verified] xnu-12377.121.6 `osfmk/arm64/pcb.c:401–425`; LLVM `AArch64ISelLowering.cpp:33570`; measured on macOS 27 and with spike S3 on the iPhone (iOS 27) |
| F4 | The kernel zeroes `x18`, except with an Apple-private entitlement. | Guest code cannot keep a value in `x18`. LLVM reserves `x18` for Android only from version 9; older builds such as Unity ≤ 2020.3 (NDK r19) use it as a normal register. The converter puts each of their uses into an island that keeps the value in TSD slot 11 (ADR-0020). | [verified] xnu `locore.s`, `machine_task.c`; LLVM `AArch64TargetParser.cpp:145` |
| F5 | Darwin takes the syscall number from `x16`, Linux from `x8`. | An unchanged Linux `svc #0` triggers a random Darwin syscall. Every `svc` must be replaced at import. | [verified] xnu `osfmk/arm64/sleh.c` |
| F6 | arm64 programs need a hard 4 GiB `__PAGEZERO`. Nothing can be mapped below 4 GiB. | Guest code comes into our process as a dylib. ART's 32-bit object references (heap below 4 GiB) need heap-relative references in M4. | [verified] xnu `bsd/kern/mach_loader.c:1087`; ARTBox ADR 0031 |
| F7 | iOS has 16 KB pages. NDK r28+ aligns `.so` files to 16 KB by default. | `.so` files aligned to 4 KB (older games) need special handling or are rejected (ADR-0007). | [verified] developer.android.com "16 KB page sizes"; NDK r28 changelog |
| F8 | Apple's ARM64 ABI differs from AAPCS64: varargs are always on the stack, `va_list` is `char*`, the caller extends arguments below 32 bits, `long double` is 64 bits wide, `char` is signed, stack arguments are packed. | No variadic calls across the boundary. Small integer types need thunks. The guest world keeps the Android ABI completely (ADR-0011). | [verified] Apple "Writing ARM64 code for Apple platforms" |
| F9 | errno, signal and flag values differ, for errno 54 of 87. Structures (`stat`, `dirent`, `sigaction`, `ucontext`), pthread type sizes and the `FILE` layout differ as well. | We translate at one narrow point (syscall layer) instead of in hundreds of libc functions (ADR-0009). | [verified] Header comparison of Bionic UAPI against the iOS 27 SDK |
| F10 | Darwin `CLOCK_MONOTONIC` keeps counting during sleep, Linux `CLOCK_MONOTONIC` does not. | Guest `CLOCK_MONOTONIC` is mapped to Darwin `CLOCK_UPTIME_RAW`, `CLOCK_BOOTTIME` to Darwin `CLOCK_MONOTONIC`. | [verified] `man clock_gettime` (macOS 27) |
| F11 | iOS has no subprocesses. App extensions are separate processes, but have a much smaller memory limit and cost App IDs. | A game runs in the foreground process. No extensions (ADR-0003). | [verified] Apple Extensibility Guide; SideStore |
| F12 | Memory on the 15 Pro Max under iOS 27: 3.29 GiB available without the entitlement, 6.00 GiB with `increased-memory-limit`. Address space: 7.84 GiB without (largest region 6 GiB), 65.80 GiB with the entitlement (largest region 64 GiB), without `extended-virtual-addressing`. | The guest budget is at most 5–6 GB. Allocator reservations are budgeted centrally (ADR-0012). | [verified] Spike S2 on the iPhone, 2026-09-22 (with the entitlement, 1 run); before that LiveContainer issue #1559 |
| F13 | Free account: profiles are valid for 7 days, 3 apps per device, 10 App IDs per 7 days. No Extended Virtual Addressing. | One bundle ID, no extensions. The app is re-signed weekly (ADR-0015). | [verified] Apple "Compare memberships", "Supported capabilities" |
| F14 | ANGLE-Metal supports at most OpenGL ES 3.0. `OES_EGL_image_external` and `eglPresentationTimeANDROID` are missing. | Unity 6 (requires ES 3.1) and UE5 (ES 3.2) do not run on ANGLE-Metal. Vulkan becomes the main path (ADR-0013). | [verified] ANGLE `DisplayMtl.mm`, `mtl_common.h` |
| F15 | MoltenVK 1.4.2 has neither geometry shaders nor transform feedback. Swapchains exist only in BGRA8, RGBA16F and 10 bit, Android expects RGBA8. | The Vulkan shim converts RGBA8 to BGRA8 (one blit per frame). Titles with geometry shaders are incompatible. | [verified] MoltenVK `MVKDevice.mm`; AOSP `swapchain.cpp` |
| F16 | The iOS 27 SDK requires a launch screen. Xcode 27 has removed `ld64`. | `UILaunchScreen` goes into the Info.plist. We write the Mach-O containers ourselves (ADR-0008). | [verified] Release Notes iOS 27 / Xcode 27 |
| F17 | Signal delivery works without Mach exception ports: `EXC_BAD_ACCESS` becomes `SIGSEGV`/`SIGBUS` with `si_addr`, `sigaltstack` is respected. | The games' crash handlers and ART's implicit null and stack checks are feasible. This needs a translation Darwin `mcontext` ↔ Linux `ucontext`. | [verified] xnu `bsd/uxkern/ux_exception.c`, `bsd/dev/arm/unix_signal.c` |
| F18 | dyld 27 loads a dylib only with `LC_UUID` and a dependency on libSystem. `codesign` writes `LC_CODE_SIGNATURE` directly after the load commands, over whatever is there. | Every container has both, a content UUID and 256 bytes of space after the load commands (ADR-0019). | [verified] dlopen error messages on macOS 27; our own test `converter.container`, 2026-09-22 |

## 3. Overview

```
Launcher mode                           Game mode (a process start of its own, ADR-0003)
-------------                           ------------------------------------------------
SwiftUI: library, import, settings      Overlay: quit, FPS
Importer:                               GUEST WORLD (Android ABI, loaded as signed Mach-O dylibs)
  parse and check APK/splits              the app's .so (rewritten + signed at import)
  scan and rewrite .so                    libc++_shared and others from the APK
  build Mach-O containers                 Bionic: libc, libm, libdl
  sign, store                             front libs of the NDK system APIs:
                                            libEGL, libGLESv2, libvulkan, libandroid, liblog,
                                            libaaudio, libOpenSLES, libz, libnativewindow ...
                                          from M4 on: ART + framework classes
                                               |                      |
                                          syscall veneers        ABI thunks / direct binding
                                               v                      v
                                        HOST WORLD (Darwin ABI, part of the app binary)
                                          loader/linker (ELF semantics, dlopen/dlsym)
                                          Linux personality: VM, VFS, threads, futex,
                                            signals, time, /proc, system properties
                                          ANGLE-Metal | MoltenVK | CAMetalLayer
                                          AudioUnit | UITouch/GameController | CADisplayLink
```

**Import (launcher mode):**
1. Take in the file via "+" or "Open with": as a clone into the hidden folder `Library/Application Support/incoming/`, the original stays where it is (ADR-0018).
2. Parse the APK: ZIP, binary `AndroidManifest.xml`, `resources.arsc`, signature blocks v1 to v3.1.
3. Check compatibility:
   - Is `arm64-v8a` present?
   - Are splits missing?
   - GLES version and Vulkan level;
   - GMS dependency, minSdk/targetSdk.
4. Scan every arm64 `.so` (from M2 at the latest):
   - PT_LOAD layout and alignment;
   - relocation types, TLS model;
   - counters for `TPIDR_EL0` reads/writes, `svc` and `x18`.

   The result is "Ready" or "Incompatible: <reason>".
5. From M2: rewrite, generate the Mach-O container, sign. The original ELF is kept so that it can be re-signed when the certificate changes.

**Start (game mode):**
1. The launcher remembers the package and restarts the app.
2. The host world is initialized: memory budget, VFS roots, signal handlers.
3. The guest system libraries are loaded. They are in the app bundle and already signed during the Xcode build.
4. The app's `.so` files are loaded: `dlopen` of the containers, data relocations, constructors.
5. Start of the game: in M3 the NativeActivity host calls `ANativeActivity_onCreate`, from M4 ART starts the `Activity`.
6. Quitting via the overlay ends the process, then the app restarts into the launcher.

## 4. Components

### 4.1 App (`app/`, SwiftUI)
- **Tasks:** library grid, import, launcher, overlay.
- **Info.plist:**
  - `UILaunchScreen` (mandatory with the iOS 27 SDK);
  - from M1: `LSSupportsOpeningDocumentsInPlace`, `CFBundleDocumentTypes`, `UTImportedTypeDeclarations`, no `UIFileSharingEnabled` (ADR-0018);
  - from M3: `CADisableMinimumFrameDurationOnPhone`, `GCSupportsGameMode`, category Games.
- **Storage locations:**
  - `Documents/` stays empty; only iOS puts copies into `Inbox/` on "Open with", which the app takes in and deletes;
  - `Library/Application Support/incoming/` for chosen files until installation;
  - `Library/Application Support/apps/<package>/` for installed content (APKs, converted and signed libs, metadata);
  - `Library/Application Support/data/<package>/` for app data (see 4.5).
- **Bundle and signature:** one bundle ID, no app extensions (ADR-0003, ADR-0015).

### 4.2 Importer (`importer/`)
- **APKKit** (Swift package):
  - ZIP, AXML, ARSC, APK signatures v1–v3.1 (store the certificate, some apps check their signature).
  - Data model from the start as *App = base + n splits + OBBs*.
  - Compatibility rules.
  - Tests run with `swift test` on the Mac. The test oracles are androguard and apk-info (both Apache-2.0, only for comparison).
- **Converter** (C++, CMake, `importer/converter/`, on the Mac also as the tool `apk-convert`):
  - Parts: ELF scanner, rewriter (4.4) and Mach-O writer (ADR-0008).
  - The same code runs on the Mac (M2a, build tool) and on the iPhone (M2b).
- **Signer** (M2b, ADR-0022): our own code in the converter (`codesign.h`), without OpenSSL. It writes signatures like `codesign`; Security.framework makes the RSA signature. The certificate is in the iOS keychain; the signer takes the chain from the app's signature. During development, `tools/deploy.sh` puts it there: the certificate Xcode signs the app with. The app reaches the converter and signer via `APKConvert` (C interface, `build/APKConvert.xcframework`).

### 4.3 Loader (`runtime/loader`)
- **Linker with Android semantics:**
  - load groups, eager binding;
  - symbol versions following AOSP rules, GNU and SysV hash;
  - RELA/RELR/APS2, IRELATIVE, TLSDESC/IE;
  - `init_array` order;
  - `dlopen`/`dlsym`/`dladdr`/`dl_iterate_phdr` (the last two for libunwind and C++ exceptions);
  - no unloading.
- **Signed code stays unchanged:** At runtime, only data areas are written. `.so` files with text relocations are incompatible.
- **Basis:** reviewed ARTBox modules (MIT): ELF parser, relocation engine, load groups, symbol versions (ADR-0017). Everything that prebuilt `.so` files need is added (≥ 3 PT_LOADs, APS2, IRELATIVE, IE TLS), as well as growing via `dlopen`.

### 4.4 Rewriting at import (ADR-0010)

| Pattern | Occurrence | Handling |
|---|---|---|
| `mrs xN, TPIDR_EL0` (read) | practically every `.so`: the stack protector is the NDK default, ELF TLS from minSdk 29 | Becomes a branch to an island. The island loads the guest thread pointer from Darwin TSD slot 6 (`mrs xN, TPIDRRO_EL0; bic xN, xN, #7; ldr xN, [xN, #0x30]`) and branches back (ADR-0019). It costs 1.9 cycles per read on the iPhone (ADR-0020 X6); a fast path for the stack guard pattern is left for M6. |
| `msr TPIDR_EL0, xN` (write) | libc and runtimes | Becomes an island that writes the same slot, with a helper register briefly saved on the stack (ADR-0019). In our Bionic also solved with a source overlay. |
| `svc #0` | rare: Go runtime, LSS, statically linked libc | Becomes a branch to a veneer: save registers, call the Linux syscall layer, keep the Linux error convention. |
| `x18` as a register | Builds with clang ≤ 8 (Unity ≤ 2020.3: `libunity.so`, `libil2cpp.so`), old SDK binaries, ShadowCallStack | Every use becomes an island that keeps the value in TSD slot 11 (ADR-0020): on the iPhone 1 to 4 cycles more per pass, 1.5 among other work, a jump table through `br x18` 6.7. "Incompatible" only for forms without an island, such as `blr x18` |
| Text relocations | rare | "Incompatible" |

- **Island areas:** They are in the container before and after the code, because a branch only reaches ±128 MB. `.so` files with more than about 256 MB of code need a special approach and are marked incompatible for now.
- **No emulation:** Only unambiguously decoded patterns are replaced, everything else runs natively and unchanged. There is no trapping and no patching at runtime.

### 4.5 Guest libc and Linux personality (ADR-0009)
- **Guest libc:** Real AOSP Bionic (`android-15.0.0_r1`), built with the NDK following Bionic's own `Android.bp` and shipped as a guest library (ADR-0021). The overlay is small because the rewriter already covers TLS accesses and syscalls:
  - `setjmp.S` without ShadowCallStack and without `x18`;
  - additionally in `libc.so` (version `APKRT_PRIVATE`): `__apkrt_start_main_thread` sets up the main thread like linker64, `__apkrt_register_tls_module` registers dynamic TLS, plus linker64's TLSDESC resolver and `__apkrt_sigreturn` as the restorer instead of the vDSO trampoline;
  - our own `ld-android.so` with `libc_shared_globals` and the `__loader_*` functions, which the loader serves (`dl.cpp`: `dlopen` at runtime, `dlsym`, `dladdr`, `dl_iterate_phdr`). Like linker64 with `g_dl_mutex`, it serializes libdl with a recursive lock of its own, so a thread waits for another one's `dlopen` in guest code, where signals reach it (ADR-0025 d). The loader searches for libraries as linker64 does for a program in `/data/local/tmp`: first in `LD_LIBRARY_PATH`, then in `/system/lib64`. Every location without the file leaves ENOENT in errno, like a failed `open()` on Android. If a program cannot be linked, it ends as with linker64 (`CANNOT LINK EXECUTABLE`, status 1);
  - our own small `liblog.so` (NDK API, output to stderr in the format of `logcat -v tag`) and `libnetd_client.so` (Bionic opens it at startup; without netd it changes nothing).
  - `/system` contains Bionic's time zone file, packed from the Mac's IANA data (`tools/make-tzdata.py`).
- **Syscall layer (host):** a mini kernel in userspace. Linux numbers are never passed through to Darwin.

| Area | Implementation |
|---|---|
| Memory | our own address space manager: `mmap`/`munmap`/`mprotect`, `MADV_DONTNEED` by remapping, `mremap` reimplemented; never an executable mapping |
| Files | VFS with path mapping (table below, `vfs.cpp`); `openat`, `read`/`write`, `getdents64`, the `stat` family and the other file syscalls with Linux flags, structures and error codes (implemented in M2 step 5c). The guest's own descriptor table with Linux's numbers, whatever the process holds on the host, and RLIMIT_NOFILE 32,768 as on Android 15; waits on descriptors in the thread's kqueue, which a signal always ends; `eventfd`, `epoll`, `ppoll` and `pselect6` on host pipes and kqueues (ADR-0027, M3 step 1) |
| Threads | `clone` creates a Darwin pthread that starts in the parent's register state at the `svc`; the kernel loads the state via a signal that the thread sends to itself. `exit` returns to the host stack; only there is the thread ID cleared and a stack that the thread freed itself actually released. futex with the runtime's own wait queue; every blocking syscall sleeps with `os_sync_wait_on_address` (public, iOS 17.4+) on the thread's signal word, so that a signal always ends it (ADR-0026) |
| Signals | Guest signals fully emulated (`signals.cpp`): dispositions, masks, pending and queued signals, altstack. Delivery with a Linux `rt_sigframe` (siginfo, ucontext, FPSIMD, ESR) on the guest stack or guest altstack, `rt_sigreturn` restores the context. Host signals only for hardware faults (translated), for nudging other threads (SIGURG) and for terminating with a default action |
| Time | Clock mapping from F10 |
| Network | Sockets on BSD sockets: not implemented yet; the descriptor table has room for them (`epoll` and `eventfd` are in the "Files" row) |
| System | `/proc/self/{maps,cmdline,exe,auxv}`, `/proc/cpuinfo` synthetic; `AT_PAGESZ = 16384`; property area (`ro.build.version.sdk` and others) matching the shipped Bionic |

| Android path | Target on the iPhone |
|---|---|
| `/data/data/<pkg>`, `/data/user/0/<pkg>` | `Library/Application Support/data/<pkg>/data` |
| `/sdcard/Android/data/<pkg>` | `…/data/<pkg>/external` |
| `/sdcard/Android/obb/<pkg>` | `…/data/<pkg>/obb` |
| `/data/app/<pkg>-*/base.apk` (read-only) | `…/apps/<pkg>/<version>/base.apk`, the installed version's folder (ADR-0034); `…/apps/<pkg>/base.apk` for an app installed before version folders |
| `/system/lib64` | Guest system libraries as containers in the app bundle (`apkrt_add_library_directory`); `libc.so`, `libm.so` and `libdl.so` are named, as on Android 15, after their real path in `/apex/com.android.runtime/lib64/bionic/` |
| `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom` | the host's devices (`/dev/random` never blocks, as since Linux 5.6) |
| `/system/usr/share/zoneinfo/tzdata` | Bionic's time zone file, packed from the Mac's IANA data at build time |

### 4.6 ABI boundary guest ↔ host (ADR-0011)
- **Paths from guest to host:** guest code reaches the host only via syscall veneers or via imports of the front libs.
- **Binding:** The loader binds a front lib function directly to the host function if the signature is the same in both ABIs: only pointers and integers ≥ 32 bits, at most 8 registers, not variadic. Otherwise the call goes through a generated thunk that extends small integers and repacks stack arguments. The tables are generated from Khronos XML and NDK headers.
- **Forbidden across the boundary:** variadic calls, `va_list` and `long double`.
- **Host → guest** (audio callback, Choreographer, input): These calls go through thunks. When needed, the thunks register the host thread as a guest thread (guest TCB, TLS, signal mask), similar to JNI `AttachCurrentThread`. Without this registration the guest code crashes immediately, because its stack protector finds no guest thread pointer.
- **errno and time:** Host errors are translated into Linux errno. Time values follow the clock mapping from F10.

### 4.7 Graphics (ADR-0005, ADR-0013)
- **`ANativeWindow`:** a UIView with `CAMetalLayer` (ADR-0032). The window's size in pixels is the layer's bounds × `contentsScale` (ADR-0030); the front libs set the drawable size to the buffer size of `setBuffersGeometry`, or to the window's size while none is set, and Core Animation scales it onto the layer, as SurfaceFlinger does. This is later the starting point for MetalFX upscaling (M6).
- **Calls into host code** (ADR-0030): every call of a front lib goes through the runtime's entry `apkrt_host_call`, so no guest signal is delivered inside ANGLE or MoltenVK; the front libs cut waits into slices of at most 10 ms. The generator `tools/frontgen/` makes the stubs from `vk.xml`, `gl.xml` and `egl.xml`: plain, with small integers extended, or with stack arguments repacked (refines ADR-0011).
- **EGL/GLES** (ADR-0029): the front libs go to our own ANGLE build with the Metal backend only (ES ≤ 3.0), pinned to `chromium/8037`. Only `libGLESv2` is built, which carries EGL too; the app embeds it as a framework (4.43 MB installed) and opens it with `dlopen` when a guest first loads a GLES library. Patches: a fixed window size (0001), Metal's internal shaders compiled at runtime on macOS too (0002), a gate before every command buffer commit (0003), a build fix (0004). Later: `eglPresentationTimeANDROID` for Swappy (M4), `OES_EGL_image_external`. ES 3.1 comes as a prototype via ANGLE-Vulkan on MoltenVK (M4).
- **Vulkan** (ADR-0030):
  - `libvulkan` goes to MoltenVK 1.4.2 as released, only on the device: no Vulkan in the Simulator. No Khronos loader and no layers.
  - It answers as Android 15's loader does: version 1.3, Android's extension lists and formats. `VK_KHR_android_surface` becomes `VK_EXT_metal_surface`, and `libvulkan` enables `VK_KHR_portability_subset` itself.
  - Swapchains: FIFO, 3 to 4 images, the identity transform; RGBA8 is copied to BGRA8 with a blit at every present, BGRA8 goes straight to MoltenVK.
  - `AHardwareBuffer` and the fd extensions come later.
- **Background:** iOS forbids GPU work in the background. From the start of the host's background handling, a gate holds every commit of ANGLE and every Vulkan submit, present and acquire; the host waits at most 1 s until everything committed is scheduled, then the window's gate becomes "gone", and only then does the app get onPause and the other callbacks (ADR-0029 decision 15, ADR-0032).

### 4.8 Frame pacing, input, audio
- **Choreographer:**
  - CADisplayLink runs on its own thread and wakes ALooper via fds.
  - Settings: `preferredFrameRateRange` (80, 120, 120) and `CADisableMinimumFrameDurationOnPhone`.
  - Timestamps are based on `mach_absolute_time`.
- **Swappy** (statically linked in many NDK games) needs:
  - a reported SDK ≥ 31;
  - working JNI methods `Display`/`WindowManager`;
  - `VK_GOOGLE_display_timing` (MoltenVK has it);
  - `eglPresentationTimeANDROID` (ANGLE patch).
- **Input:**
  - `UITouch` including coalesced touches becomes `AMotionEvent` with history, pointer IDs and pixel coordinates.
  - `GCController` becomes `AKeyEvent`/`AMotionEvent` with the Android axes.
- **Audio:**
  - Our own `libaaudio` on RemoteIO. It must export the symbol set that Oboe loads.
  - OpenSL ES comes from AOSP `frameworks/wilhelm` (Apache-2.0) with a new AudioUnit backend.

### 4.9 ART and framework (M4, outlook)
- **Goal: ART as a guest library with the Android ABI**, built as for a device. This keeps JNI calls with their varargs (`Call*Method(...)`) in one ABI, and through the personality ART sees the same Linux semantics as the rest. The alternative, building ART for Darwin, is decided in M4.
- **Configuration:**
  - interpreter (`-Xint`, no JIT);
  - imageless boot from the boot jars;
  - GC without userfaultfd (no CMC);
  - **heap-relative 32-bit references** because of F6 (ARTBox approach, so far only tested in isolation).
- **nterp** does not generate code at runtime, but is part of `libart` [inferred]. So it does not fall under the JIT ban, but must be adapted to the heap-relative references.
- **Framework:** Like ATL, we cut between the app and the Java framework: the framework is reimplemented on real ART, cleanly rewritten without ATL code. No Binder processes, system services are in-process objects.
- **License:** The project license is Apache-2.0 (ADR-0017). libcore is under GPL-2.0 with the Classpath Exception and therefore stays a separate component.
- **M6:** The dex2oat output goes through the same pipeline as `.so` files (convert, sign, `dlopen`). For this, the compiler must generate heap-relative references.

### 4.10 Process model (ADR-0003)
- **Restart:** The launcher writes the target package into the UserDefaults, opens its own app via `LSApplicationWorkspace` (private SPI, like LiveContainer) and exits. At launch, the app reads the target and boots straight into game mode.
- **Fallback:** the app exits, and the user taps the icon again.
- **Why restart:** only one game runs per process, because loaded libs and ART cannot be unloaded cleanly.

## 5. Reference projects: what we adopt, what we don't

| Project (version) | License | Adopt | Do not adopt | Why |
|---|---|---|---|---|
| ARTBox (`fd48d48`, 2026-09-14) | MIT | After review, as pinned modules: ELF parser, relocation engine, load groups, symbol versions, VM/futex/VFS engines. Plus the Bionic build recipe (overlay of 6 files, syscall stubs), the test discipline (oracles, fixed denominators) and from M4 the ART recipe. | Build-time-only wrapper as the product path; controlled ELF layout (`--no-relax`, exactly 2 PT_LOADs); TLS trick only in source code; Scudo with 8.25 GiB VA; forgoing device tests | Best starting point. But: 9 days old, one author, never ran on an iPhone, only meant for self-built ELFs. |
| LiveContainer (`4dbe0f9`) | AGPL-3.0 | Concepts only: certificate import from SideStore/AltStore, co-signing of all Mach-Os, `dlopen` from the data container, restart in place, `seg_count` tolerance for iOS 27 | Code; multitasking via app extensions; private dyld hooks | License; memory limits of the extensions |
| zsign (upstream, `614caa8`) | MIT | Only read for comparison: which blobs and attributes a signature needs | Code; the fork from Feather/LiveContainer | Our own signer without OpenSSL (ADR-0022); Feather is GPL-3.0 |
| Android Translation Layer (`3e686d1`) | GPL-3.0 | Concepts only: cut app ↔ Java framework, NativeActivity host via `dlopen`+`dlsym`, inserting surface and instance extensions for Vulkan, path virtualization | Any code; GTK/Wayland/ALSA | License; wrong platform |
| bionic_translation (`b36c17d`) | Apache-2.0, linker without a license (from android2gnulinux) | Idea of a TLS module registry | Code; the shim approach | Unclear origin; on Darwin all constants differ (F9) |
| art_standalone (`66a5d90`) | Apache-2.0 | Reference for how ART is built outside AOSP | The version itself (Android 10 era, GCC/Linux) | Outdated |
| AOSP (Bionic, ART, libcore, wilhelm, apksig, gamesdk) | Apache-2.0/BSD; libcore GPL-2.0+CE | Bionic, ART, wilhelm; 4 KB compatibility logic from `linker_phdr_16kib_compat.cpp`; signature formats | – | Reference implementation |
| StikJIT (`3623e72`) / StikDebug | MPL-2.0 / AGPL-3.0 | Later, only for an optional JIT path: URL protocol `stikdebug://enable-jit`, `brk #0xf00d` | Built-in StikJIT (needs an extension) | ADR-0002 |
| bundletool / apkeep | Apache-2.0 / MIT | References for M5/M7 | – | – |
| ANGLE / MoltenVK | BSD-3 / Apache-2.0 | Dependencies from M3, only after asking | MetalANGLE (inactive), Apple's EAGL (deprecated, no EGL) | – |
| KosmicKrisp | MIT | Watch: Vulkan 1.3 conformant, has transform feedback | Not at all for now | No iOS support (as of 2026-08) |

## 6. Dependencies and effort (added only after asking)

| Dependency | From | Size / effort (estimated) | License |
|---|---|---|---|
| XcodeGen 2.46.0 | M0 | Homebrew, small | MIT |
| CMake 4.4.3, Ninja 1.13.2 | M2 | Homebrew, small | BSD-3 / Apache-2.0 |
| Android NDK r30 (30.0.16248370) | M2 | Download approx. 1.07 GB | Apache-2.0 and others |
| Android emulator, platform tools, system image Android 15 (without Android Studio) | M2, reference (ADR-0014) | Download 1.2 GB, unpacked 2.9 GB; installed 2026-09-22 | Apache-2.0 and others |
| ~~zsign + OpenSSL~~ | M2b | dropped: our own signer with Security.framework (ADR-0022) | – |
| AOSP Bionic (pinned tag) | M2 | Source archive, build with the NDK | BSD/Apache |
| MoltenVK 1.4.2 | M3 | Khronos release as it is (ADR-0030): iOS tar 34.5 MB, macOS tar 59.6 MB (for the tests on the Mac), download approved 2026-09-25 | Apache-2.0 |
| ANGLE `chromium/8037` | M3 | our own build (ADR-0029): about 113 MB fetched, 866 MB in `build/angle/`, about 48 s per slice with 6 cores; `libGLESv2.framework` 4.43 MB installed on iOS | BSD-3 and its parts (`THIRD_PARTY.md`) |
| ART + libcore + ICU | M4 | approx. 1,150 C++ units (ARTBox count), JDK 17 + D8, `libart` approx. 14 MB, ICU data approx. 28 MB | Apache-2.0; libcore GPL-2.0+CE |

## 7. Risks

| ID | Risk | Level | Mitigation |
|---|---|---|---|
| R1 | Thread pointer and TLS in all prebuilt `.so` files (F3) | high | Rewriter (ADR-0010); spike S3 confirmed the XNU behavior on the iPhone |
| R2 | Converting, signing and `dlopen` of Android code are unproven on the iPhone | done for the test suite (M2b, 2026-09-22) | S1b: a Mac-signed dylib with the bundle ID as identifier loads from the data container. M2a loads from the bundle (step 6). M2b: the app converts and signs with the imported certificate, loads from `Application Support`; 13 of 13 identical (ADR-0022). Open: large libraries of real APKs (R8, R17, ADR-0020) |
| R3 | Memory and address space with a free account (F12, F13) | high; medium with the entitlement | `increased-memory-limit` (S2: 6.00 GiB, 64 GiB region); allocator reservations budgeted centrally (ADR-0012) |
| R4 | ART heap above 4 GiB (F6), interpreter slow | high (M4/M6) | Heap-relative references; games with little Java (NativeActivity, IL2CPP) first |
| R5 | GLES ≥ 3.1 not via ANGLE-Metal (F14) | high | Vulkan as the main path; prototype ANGLE-Vulkan in M4 (ADR-0013, accepted 2026-09-24) |
| R6 | No Android device: how an app runs on an Android phone stays unmeasured | low (was high) | Your decision (ADR-0014, 2026-09-24): speed against the native iOS version on the same iPhone; in M3 the benchmark scene as an APK and as a native build; the emulator only for output and behavior |
| R7 | Errors at the ABI boundary: small integers, callbacks on host threads (F8) | medium | Generated thunks; tests with garbage in the upper bits |
| R8 | `.so` files aligned to 4 KB (F7), not only in older games: in the test APK it is 9 of 16, all from SDKs (Firebase, Crashlytics); `libil2cpp.so` and `libunity.so` have 16 KB | medium | Only 16 KB at first; then RW shift plus ADRP correction, driven by corpus data (ADR-0007) |
| R9 | Signals: crash handlers of Unity/Mono/ART expect a Linux `ucontext` | medium → low | Implemented in M2 step 5c: Linux `rt_sigframe`, faults translated, handlers may change the context (checked by `signal-test`); 3.5 µs per delivery |
| R10 | dyld internals and private SPIs change (iOS 27: `seg_count`) | medium | Our own Mach-O writer without chained fixups; keep SPI use small and encapsulate it; LiveContainer issues as early warning |
| R11 | Uncertain state of ARTBox's code | medium | Only individual modules, pinned, reviewed, with our own tests |
| R12 | Certificates from Xcode and SideStore collide. The limit on active development certificates can lead to a revocation, and with that, everything signed with it becomes invalid. | medium | During development only Xcode; `tools/deploy.sh` hands over the certificate. With SideStore: detect a new certificate at the leaf of our own signature, adopt it (as of SideStore 0.6.4 from `ALTCertificate.p12` in the bundle) and re-sign everything (ADR-0022) |
| R13 | Performance cost from islands, syscall layer, swapchain blit and interpreter ART | medium | Islands measured (ADR-0020 X6): 1 to 4 cycles per pass, a thread pointer read 1.9; a syscall through the veneer 21 ns (ADR-0025); swapchain blit in M3, ART in M4; fast paths |
| R14 | New project format in Xcode 27.2 (XcodeGen #1651/#1653) | low | Stay on Xcode 27.0 until XcodeGen supports it |
| R15 | The Simulator is misleading: not the same signature check, no Jetsam, ANGLE fixed at ES 3.0 | low | Simulator only for the UI; loader tests on macOS and the iPhone |
| R16 | Licenses: AGPL/GPL references, libcore | low | ADR-0017: project license Apache-2.0 (2026-09-22); only read AGPL/GPL; libcore as a separate component |
| R17 | `x18` as a normal register in builds with clang ≤ 8: in the test APK `libunity.so` and `libil2cpp.so` (Unity 2020.3) and 4 SDK libs | low (was high) | Virtualized (ADR-0020, 2026-09-24): every `x18` site of the test APK gets an island. What the sites cost in a running engine shows its profile in M4 |

## 8. Open questions

**Your decision:**
- ~~GLES path~~: accepted as proposed (ADR-0013, 2026-09-24);
- ~~reference device~~: none, speed against the native iOS version (ADR-0014, 2026-09-24);
- when to possibly pay for the account (ADR-0015);
- bundle ID.

**Prototypes:**
- ~~signing on the iPhone~~: done with our own signer (M2b, ADR-0022), on the iPhone 13 of 13;
- frequency of 4 KB layout, `TPIDR_EL0`, `svc` and `x18` in real APKs (corpus, M1/M2). First data points from the test APK, read with `apk-convert inspect`: 4 KB layout in 9 of 16 `.so` files; `x18` read in 6 of 16 (ADR-0019, part 4);
- ~~cost of the islands~~: measured on the iPhone (ADR-0020 X6);
- fast path for the stack guard (its island costs 1.9 cycles per read, ADR-0020 X6);
- ART with heap-relative references and nterp (M4);
- cost of the swapchain blit at 120 Hz (M3).

## 9. Main sources

- **ARTBox:** github.com/j0shua-SYSON/ARTBox (`docs/loader-design.md`, `DECISIONS.md`, `STATUS.md`)
- **LiveContainer:** github.com/LiveContainer/LiveContainer (`LCMachOUtils.m`, `LCBootstrap.m`, `zsign.mm`, `LCSettingsView.swift`), livecontainer.github.io
- **SideStore** (AGPL-3.0, only read): github.com/SideStore/SideStore, tag `0.6.4` (`EmbedSigningCertOperation.swift`, `CertificateManager.swift`, `OperationStepDefinition.swift`)
- **Android Translation Layer:** gitlab.com/android_translation_layer (`doc/Architecture.md`)
- **XNU:** github.com/apple-oss-distributions/xnu, tag `xnu-12377.121.6` (`pcb.c`, `locore.s`, `sleh.c`, `mach_loader.c`, `ux_exception.c`)
- **Apple:** "Writing ARM64 code for Apple platforms", "Supported capabilities (iOS)", "Compare memberships", Release Notes iOS 27 / Xcode 27
- **LLVM:** `AArch64ISelLowering.cpp`, `AArch64TargetParser.cpp`
- **Android:** "Support 16 KB page sizes", NDK releases (r30), AOSP `bionic/linker/linker_phdr_16kib_compat.cpp`
- **Graphics:** ANGLE (`DisplayMtl.mm`, `SurfaceMtl.mm`), MoltenVK 1.4.2 (`MVKDevice.mm`, `MVKImage.mm`), Mesa KosmicKrisp docs
- All other sources with commits: [`research/`](research/)
