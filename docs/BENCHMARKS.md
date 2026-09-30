# Benchmarks

## M3: the parity benchmark (ADR-0037)

Minato's goal is parity: an APK in Minato reaches the native iOS build's speed on the same iPhone, at least 95 % of
its FPS with a p99 frame time at most 105 % of it (ADR-0014, `CLAUDE.md`). M3 is done when this file has the result.
ADR-0037 measures it with one scene built twice: as the APKs `bench-gles` and `bench-vulkan` in Minato, and as
`MinatoBench.framework`, the same C files built for iOS. Both draw through ANGLE and MoltenVK, so the difference
between them is what Minato costs.

**Result: parity does not hold yet. 5 of 6 configurations pass; `vulkan-120-heavy` fails, with the APK at 90.83 % of
the native build's FPS and its p99 at 133.33 %.** Measured 2026-09-29 to 30 on an iPhone 15 Pro Max.

| Configuration | Native FPS | APK FPS | FPS ratio | Native p99 | APK p99 | p99 ratio | Native spread FPS / p99 | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| gles-60-light | 60.00 | 60.07 | 1.0011 | 16.670 ms | 16.670 ms | 1.0000 | 0.06 / 0.00 % | pass |
| gles-120-light | 119.97 | 119.97 | 1.0000 | 8.335 ms | 8.335 ms | 1.0000 | 0.03 / 0.00 % | pass |
| gles-120-heavy | 91.47 | 91.80 | 1.0036 | 20.838 ms | 20.838 ms | 1.0000 | 3.17 / 0.00 % | pass |
| vulkan-60-light | 60.00 | 59.97 | 0.9994 | 16.670 ms | 16.670 ms | 1.0000 | 0.06 / 0.00 % | pass |
| vulkan-120-light | 119.97 | 119.97 | 1.0000 | 8.335 ms | 8.335 ms | 1.0000 | 0.00 / 0.00 % | pass |
| vulkan-120-heavy | 86.17 | 78.27 | 0.9083 | 12.503 ms | 16.670 ms | 1.3333 | 0.15 / 0.00 % | **fail** |

The table gives medians by nearest rank over the warm windows (20 s to 50 s after the first present) of 5 valid runs
per variant. The ratios are APK over native. A verdict counts only when the native runs agree within 5 % among
themselves: max − min over the median, for FPS and for p99, as decision 8 says (the "Native spread" column). Here they
agree within 3.17 %.

### Where `vulkan-120-heavy` loses

Every run shows the same result. Present intervals fall on ProMotion's steps, and the APK's intervals sit a step
higher than the native build's. It shows few 8.33 ms intervals and 7 % at 16.67 ms, so its p99 is 16.67 ms instead of
12.50 ms:

| vulkan-120-heavy | FPS, 5 runs | at 8.33 ms | at 12.50 ms | at 16.67 ms | at 20.83 ms | CPU p50 per frame |
|---|---:|---:|---:|---:|---:|---:|
| native | 86.13 to 86.27 | 21.4 to 21.8 % | 78.2 to 78.6 % | 0 % | 0 % | 4.41 ms |
| APK | 78.23 to 78.43 | 0.6 to 1.4 % | 91.3 to 92.5 % | 6.4 to 6.9 % | 0.1 to 0.5 % | 4.94 ms |

The Vulkan APK and the native build differ in several things that are Minato's:

- the loader
- `libvulkan` and its thunks (9 to 15 ns per call on the Mac, V5)
- the glue and the runtime's vsync channel
- the render thread's QoS class: default, against user-interactive
- the swapchain format: the APK asks for R8G8B8A8 (VkFormat 37), which `libvulkan` copies to the layer's BGRA8 with a
  blit at every present (F15, ADR-0030). The native build's swapchain is B8G8R8A8 (44) and has no copy.

The GLES APK shares the loader, the glue, the vsync channel and the QoS class. It has its own front libs and no copy,
and at the same load it matches the native build (1.0036 and 1.0000).

The copy is therefore the likely cause, but this series did not measure it. ADR-0037 names this risk. V5's iPhone
part, the copy's GPU time per frame, would show whether the copy is the cause. On the Mac (M4 Pro, 1290 × 2796) V5
measured 0.18 ms. The remedies stay ADR-0030's: B8G8R8A8 first through an app profile in M6, or a MoltenVK patch.

The mean interval (12.78 ms against 11.61 ms) does not give the copy's cost: a frame that misses a step waits for the
next one, so a small cost per frame moves a share of frames by a whole 4.17 ms (see Limits).

The APK's render thread also spends more CPU time per frame. Its CPU p99 of 5.45 ms is well below the 12.5 ms step,
so the GPU sets the rate [guess, from these numbers]. At the light loads the GPU has room within its step, and the same
path passes (0.9994 and 1.0000).

### How it was measured

- *Scene:* N lit, textured spheres of 768 triangles, one draw each. N alpha-blended particles in one instanced draw.
  A full-screen background, and a post pass from an offscreen target at the layer's 1290 × 2796 pixels. The motion
  follows the frame index only, so both builds draw the same frames (`graphics.bench` checks frame 1000's CRC).
- *Variants:*
  - Native: `MinatoBench.framework` in Minato's benchmark build. A render thread at QoS user-interactive draws one
    frame per tick of the app's `DisplayLinkClock`.
  - APK: the APK is imported into Minato like any APK and started in game mode. It draws one frame per
    `AChoreographer` callback through the runtime's vsync channel. Its render thread runs at QoS default (the runtime
    sets no class on guest threads).
  - Vulkan in the APK offers RGBA8 first and copies to the layer's BGRA8; natively, MoltenVK takes BGRA8.
- *Recorder:* graphics/host's `MinatoMetalLayer` is the game view's layer in both variants. It notes each drawable's
  `nextDrawable` time and its `presentedTime` (Metal's presented handler).
  - FPS = presented frames in a window over the window's length.
  - Frame times = intervals between two presents, with p50 and p99 by nearest rank.
  - Drawables that share a present time count once; the earlier ones count as drops.
  - The verdict's tool is `tools/bench-summary.py` at ccdf0c5.
- *Series:* per API, a light load at 60 and 120 Hz and a heavy load at 120 Hz.
  - Each configuration needs 5 valid runs per variant, interleaved A B B A. One warm-up pair per API comes first and is
    discarded.
  - Each run is a fresh process, launched by `devicectl` without a debugger. It waits until the thermal state is
    nominal, then draws for 52 s.
- *Validity (decision 6):* a run is valid when all of these hold:
  - it starts nominal and never goes above fair
  - Low Power Mode is off
  - the display link's median period is within 1 % of the rate
  - no debugger is attached
  - the app never loses focus
  - the layer's size and drawable count match the other variant's
  - its present times are plausible

### Calibration (decision 7)

Decision 7 wants a heavy N at which the native build draws about 90 FPS at 120 Hz. The light N is a quarter of that
and must hold 120 Hz. The ADR guessed 512 and 2048, which was too light:

- At N = 2048 (the recorder's first iPhone runs, 2026-09-29), all four variants held 120 Hz in their first 10 s
  (114 to 115 FPS, p50 8.335 ms).
- At N = 8192, the native build drew 89.3 FPS with GLES and 86.0 FPS with Vulkan in the warm window.

So the heavy N is 8192 and the light N 2048 for both APIs, fixed in `graphics/bench/scene.c` (cb483ff). In the series,
the light loads held their rate in every valid run: at least 97.9 % of the intervals were at 8.33 ms, or at 16.67 ms
for 60 Hz.

### The builds

Each half ran Release benchmark builds that `tools/benchmark.sh` made from the working tree.
`build/benchmark/tree-at-build.txt` (not in git) has a section per build with HEAD, `git status` and
`git diff --stat`.

- **GLES:** 2026-09-29 23:48 to 2026-09-30 06:17, six builds after restarts, sections 1 to 6.
  - The build is the compiled code of cb483ff.
  - Its runs' `run.csv` files name cb483ff-dirty (24 runs) and ffed77d-dirty (11). ccdf0c5 and ffed77d changed no
    compiled path.
  - The only dirty compiled path was `runtime/include/APKRuntime/apkrt.h`, a comment.
- **Vulkan:** 2026-09-30 15:55, one build, section 7.
  - The build is ffed77d with Runtime's uncommitted work on ADR-0036 c2 (32 runs, ffed77d-dirty).
  - Section 7 also has the SHA-256 of what `tools/embed-guest-system.sh` embeds without building it: `build/bionic`
    and the front libs.
  - A byte-identity check against the last GLES build compared every Mach-O without its signature, and the APKs'
    entries by CRC. These are the same: the bench APKs, `MinatoBench`, MoltenVK, ANGLE, `activity-host` and every
    guest library.
  - Two things differ. `libOpenSLES.so` is new, and neither bench APK loads it. Minato's binary was relinked after
    two header changes: a comment in `apkrt.h`, and a declaration in `guest_interface.h` that no host code uses.
  - The check's manifests are in `build/benchmark-calibration/builds/` (not in git).

### Conditions

These hold for all 67 runs, the warm-up pairs and the invalid runs included:

- Release, no debugger, Low Power Mode off, started nominal, `maximumFramesPerSecond` 120.
- The layer: 1290 × 2796 in `bgra8Unorm` with 3 drawables.
- The native render thread at user-interactive, the APK's at default.
- The thermal state at the end: nominal ×46, fair ×18, serious ×3 (the three invalid runs).

The battery was charging at 80 %, with three exceptions:

- The Vulkan half's first four runs were at 60 to 75 % while the phone charged.
- gles-120-light-apk-05 reported unplugged at 80 %; it was invalid from heat.
- vulkan-120-heavy-apk-03 reported unplugged at 80 %; it was valid, and its 78.43 FPS match the other APK runs'.

The setup:

- iPhone 15 Pro Max (iPhone16,2), iOS 27.0 (24A437); Xcode 27.0 (27A266a) on the Mac.
- The phone was on its cable, flat on a hard surface, with Do Not Disturb on, Auto-Lock never and no other app open.
- The case was off and a fan ran from 2026-09-30 about 06:00 on (details under the GLES half below).

### How the series ran

**GLES** ran in two sittings: 2026-09-29 23:48 to 2026-09-30 00:34, and 04:51 to 06:53.

- *Order:* as bench-plan gives it. First the warm-up pair (gles-120-heavy-*-00), then gles-60-light, gles-120-light
  and gles-120-heavy.
- *Pauses:*
  - none until 00:34
  - from 04:51, 5 min before each heavy run and after each run that ended above nominal
  - from 05:21, 10 min before each heavy run (gles-120-heavy-native-03 had gone serious after a 5 min pause), and
    5 min after a warm end

  A helper outside the repo held `benchmark.sh` between two runs, never within one. The case was off from 04:50 and a
  fan ran from about 06:00.
- *Invalid from heat,* each run again. Times are after the first present:
  - gles-120-light-apk-05: fair at 7.4 s, serious at 32.4 s
  - gles-120-heavy-native-01: 10.9 s and 30.9 s
  - gles-120-heavy-native-03: 36.3 s and 51.3 s
- *Invalid by the layer rule:* gles-120-heavy-apk-04 first drew in landscape (2796 × 1290) after the iPhone was
  turned for the fan. bench-plan stopped the series because the pair's layers differed. The run was moved to
  `build/benchmark/failed/` and run again in portrait, and Portrait Orientation Lock stayed on from then.
- *USB drops:* `devicectl` reported "The connection was invalidated" (Mercury error 1001), each time a few seconds
  after a run began to draw. Lost runs were run again under the same number. Their consoles are in
  `build/benchmark/failed/`.
  - gles-60-light-apk-05 at 00:04: run again.
  - gles-120-heavy-native-02 at 00:34: lost. Its orphaned run was ended with `devicectl`.
  - gles-120-heavy-native-02 at 04:55: lost.
  - gles-120-heavy-native-04 at 05:31: recovered. The app finished the run on its own, and its CSVs were fetched with
    `benchmark.sh`'s own command.
- *End:* the series stopped at 06:53, in the pause before the Vulkan half, when the user's time ended.

**Vulkan** ran in one sitting, 2026-09-30 16:02 to 18:30 (2 h 28 min).

- 32 runs, none invalid, no drop.
- Pauses: 10 min before each heavy run after the first (11 pauses), and 5 min before a light run that followed a run
  ending fair (after vulkan-120-heavy-apk-00 and vulkan-120-light-apk-02).
- Before the first run, the phone had been idle through the build and the setup.

### Cold windows and CPU time (reported, not judged)

| Configuration | Variant | Valid runs | Warm p50 | Warm CPU p50 / p99 | Cold FPS | Cold p99 | Cold drops | First frame |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| gles-60-light | native | 5 | 16.670 ms | 5.75 / 6.11 ms | 57.56 | 16.670 ms | 24 | 424 ms |
| gles-60-light | apk | 5 | 16.670 ms | 5.70 / 6.13 ms | 57.42 | 16.671 ms | 1 | 452 ms |
| gles-120-light | native | 5 | 8.335 ms | 3.62 / 4.51 ms | 114.94 | 8.335 ms | 22 | 423 ms |
| gles-120-light | apk | 5 | 8.335 ms | 3.52 / 4.36 ms | 118.56 | 8.335 ms | 1 | 121 ms |
| gles-120-heavy | native | 5 | 12.503 ms | 4.41 / 5.14 ms | 88.25 | 20.838 ms | 16 | 399 ms |
| gles-120-heavy | apk | 5 | 12.503 ms | 4.41 / 5.07 ms | 89.59 | 20.838 ms | 1 | 290 ms |
| vulkan-60-light | native | 5 | 16.670 ms | 4.36 / 4.62 ms | 58.72 | 16.671 ms | 11 | 234 ms |
| vulkan-60-light | apk | 5 | 16.670 ms | 4.61 / 4.92 ms | 58.70 | 16.671 ms | 1 | 228 ms |
| vulkan-120-light | native | 5 | 8.335 ms | 4.31 / 4.52 ms | 116.61 | 8.335 ms | 11 | 239 ms |
| vulkan-120-light | apk | 5 | 8.335 ms | 3.29 / 3.70 ms | 116.94 | 8.335 ms | 1 | 227 ms |
| vulkan-120-heavy | native | 5 | 12.503 ms | 4.41 / 5.06 ms | 80.17 | 20.838 ms | 5 | 228 ms |
| vulkan-120-heavy | apk | 5 | 12.503 ms | 4.94 / 5.45 ms | 75.35 | 20.838 ms | 1 | 202 ms |

Medians over the valid runs:

- *Cold window:* its first 10 s, plus the time from the variant's start to its first present.
- *First frame:* from the variant's start to its first present.
- *CPU:* the render thread's CPU time per frame, measured by the scene.
- *Cold drops:* drawables that never reached the screen. The native variant drops 5 to 24 in its cold window, the APK 1.

### Limits

- *What the twin does not show* (ADR-0037):
  - the cost of ANGLE and MoltenVK themselves against an engine's own Metal backend, since both builds use them (that
    shows with real apps from M4 on, ADR-0014)
  - Java and ART
  - x18 islands (the NDK's clang never uses x18; ADR-0020's X6 measured their cost)
  - the loading and memory of large apps
  - audio and input threads
  - heat over longer sessions
  - an Android phone's speed
- *ProMotion's steps:*
  - Present intervals fall on 8.33, 12.5, 16.67 and 20.83 ms, and a frame that misses a step waits for the next one.
    At a load that fills the steps, a small cost per frame therefore moves a share of frames a whole 4.17 ms, and p99
    moves in whole steps.
  - At 120 Hz a p99 ratio is 1 or at least 1.25. The 5 % tolerance on p99 in effect asks for the same step.
  - gles-120-heavy has the same distribution in both variants: 42 to 47 % at 8.33 ms, 50 to 55 % at 12.5 ms, 2 to 4 %
    at 20.83 ms, and hence a p99 of 20.838 ms.
  - vulkan-120-heavy's distributions differ by a step (see above).
- *Orientation:* the bench APKs declare no `screenOrientation`, so they turn with the device; the native variant does
  not. One landscape run was invalid by the layer rule. The rest of the series ran with Portrait Orientation Lock on.
- *Heat and pauses:*
  - Heat set the pace. Three runs went serious and were run again.
  - The heavy runs had 10 min pauses before them, and the phone lay on a cool surface with a fan. The numbers are
    those of a phone kept at fair at most.
  - The pauses and the recovery of a dropped run came from a helper outside the repo. `tools/benchmark.sh` alone runs
    the series without pauses.
- *Game Mode* is declared in every build (decision 9). Whether iOS had it on during a run is not known: no API tells.

### Reproduce

```bash
tools/benchmark.sh
python3 tools/bench-summary.py build/benchmark
```

`tools/benchmark.sh` builds the Release benchmark build, installs it and runs the series in bench-plan's order. A
stopped series goes on where it stopped. The results land in `build/benchmark/<run>/` (`run.csv`, `frames.csv`,
`scene.csv`), and the summary in `build/benchmark/summary/`.
