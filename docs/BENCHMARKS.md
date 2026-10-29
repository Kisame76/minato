# Benchmarks

## M3: the parity benchmark (ADR-0037)

Minato's goal is parity: an APK in Minato reaches the native iOS build's speed on the same iPhone, at least 95 % of
its FPS with a p99 frame time at most 105 % of it (ADR-0014, `CLAUDE.md`). M3 is done when this file has the result.
ADR-0037 measures it with one scene built twice: as the APKs `bench-gles` and `bench-vulkan` in Minato, and as
`MinatoBench.framework`, the same C files built for iOS. Both draw through ANGLE and MoltenVK, so the difference
between them is what Minato costs.

**Result: parity does not hold yet. 5 of 6 configurations pass; `vulkan-120-heavy` fails, with the APK at 90.83 % of
the native build's FPS and its p99 at 133.33 %.** Measured 2026-09-29 to 30 on an iPhone 15 Pro Max. V5 then measured
the copy that `libvulkan` makes at every present: it explains about half of the gap, and without it the APK still
fails on p99 (see [V5](#v5-the-copy-on-the-iphone-adr-0030)). V5's QoS A/B then ruled out the render thread's QoS
class as the rest. Its queue-path A/B found that `libvulkan`'s queue path costs about a third of the rest and none of
the late frames. Its late-frames A/B found the rest: the APK's layer was not opaque. With the layer opaque and
without the copy, the APK is the queue path's 0.17 ms a frame from the native build, with its p99 and no late frames.

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

The copy was therefore the likely cause, but this series did not measure it; ADR-0037 names this risk. V5 below
measured it: the copy explains about half of the gap, and the rest lies elsewhere in the Vulkan APK's path, not in
the render thread's QoS class.

The mean interval gives the cost of the APK's path per frame, though not the copy's alone: 12.78 ms against 11.61 ms,
1.17 ms more. The step shares fit a pipeline that the GPU keeps busy, whose frame period is the mean interval. A
period of 11.61 ms puts 21.6 % of the intervals at 8.33 ms and the rest at 12.50 ms; one of 12.78 ms puts 6.7 % at
16.67 ms and the rest at 12.50 ms. Both are as measured. A frame that misses a step waits for the next one, and the
frames queued behind it catch up, so the mean stays the period.

The APK's render thread also spends more CPU time per frame. Its CPU p99 of 5.45 ms is well below the 12.5 ms step,
so the GPU sets the rate [guess, from these numbers]. At the light loads the GPU has room within its step, and the same
path passes (0.9994 and 1.0000).

### V5: the copy on the iPhone (ADR-0030)

**Result: the copy explains about half of `vulkan-120-heavy`'s gap. Without it the APK reaches 96.03 % of the native
build's FPS, but its p99 stays a step or two higher (16.670 and 20.838 ms against 12.503 ms), so parity still
fails.** Measured 2026-09-30 from 19:37 to 21:08 on the same iPhone.

By the rule fixed before the runs, the result is "between". B reaches 95 % of the control's FPS but not its p99, and
it is 5.5 % above A. The copy's share of the gap, (B − A) / (N − A), is 0.56 on the second build and 0.49 over all
runs.

| Run | Build | Arm | FPS | p99 | at 8.33 / 12.50 / 16.67 / 20.83 ms | CPU p50 | Period |
|---|---|---|---:|---:|---|---:|---:|
| native-50 | 26d724f | N | 86.07 | 12.503 ms | 21.1 / 78.9 / 0.0 / 0.0 % | 4.39 ms | 11.62 ms |
| apk-51 | 26d724f | A | 80.37 | 20.838 ms | 7.5 / 88.5 / 1.7 / 2.2 % | 4.79 ms | 12.45 ms |
| apk-52 | 26d724f | A (meant as B) | 80.47 | 16.670 ms | 5.2 / 92.3 / 1.5 / 1.0 % | 4.78 ms | 12.43 ms |
| apk-53 | 8109689 | B | 83.20 | 16.670 ms | 13.1 / 85.5 / 1.3 / 0.1 % | 4.40 ms | 12.02 ms |
| apk-54 | 8109689 | A | 78.33 | 16.670 ms | 1.4 / 91.3 / 6.8 / 0.5 % | 4.92 ms | 12.77 ms |
| apk-55 | 8109689 | A | 78.37 | 16.670 ms | 1.8 / 90.9 / 6.7 / 0.6 % | 4.89 ms | 12.76 ms |
| apk-56 | 8109689 | B | 82.10 | 20.838 ms | 12.4 / 84.9 / 0.8 / 1.9 % | 4.47 ms | 12.18 ms |

The runs are `vulkan-120-heavy-<run>`, each cut like the series (the warm window, `tools/bench-summary.py`'s `Run`).
The period is the mean present interval, the frame's cost (see above). By period, on the second build, the copy
costs 0.67 ms a frame (A's 12.77 ms against B's 12.10 ms), and the rest of the APK's path 0.48 ms (B against N's
11.62 ms).

**Method.** One sitting, both arms on each build, a native control first (the lead's OK of 2026-09-30):

- *A:* the Vulkan APK as in the series. It takes the surface's first format, R8G8B8A8 (37), and `libvulkan` copies
  each frame to the drawable.
- *B:* the same APK started with `MINATO_VULKAN_BGRA8_FIRST=1` in its guest environment. `libvulkan` then lists
  B8G8R8A8 (44) first, the APK takes it, and it draws straight into MoltenVK's drawable, as the native build does.
  Only the copy differs from A. Each run's console names the format it took.
- *N:* the native build, as in the series.
- *The rule, fixed before the runs:* if B reaches at least 95 % of N's FPS and its p99 is within 105 % of N's, the
  copy is the cause. If B is within 1 % of A, the copy is ruled out. In between, the copy's share is
  (B − A) / (N − A).

**The builds.** Release benchmark builds that `tools/benchmark.sh --api vulkan` made and installed. Its series was
complete, so it started no run. `build/v5/tree-at-build.txt` (not in git) has a section for each build, and the
runs' `run.csv` files name both commits with -dirty (the switch, uncommitted).

- *First, 19:09:* 26d724f with the switch.
  - Against the series' Vulkan build, these are byte-identical: the bench APKs, `MinatoBench`, MoltenVK, ANGLE,
    `activity-host`, `libGLESv2` and `libGLESv3`.
  - `libEGL` differs only in its build ID, and `libvulkan` has the switch.
  - `libaaudio`, `libOpenSLES` and Minato's binary carry the commits since the series (ADR-0036 c2 and App's), and
    the switch's launch argument.
- *Second, 20:23:* 8109689, the fix below. Only `libc.so` differs from the first build. Minato's binary and
  `MinatoBench` are byte-identical, so native-50 is the control for both builds.

**A bug found.** The first B, apk-52, took format 37 because no guest saw its start environment.

- The runtime's own start of libc (`runtime/bionic/src/libc_start.cpp`, in place of linker64) never set `environ`.
  linker64 sets it through `__libc_init_AT_SECURE(args.envp)`.
- `LD_LIBRARY_PATH` worked only because the runtime's loader reads it itself.
- The driver stopped at its format check, so apk-52 counts as a second A.
- 8109689 fixes it, red first: `fixture.libc-test` now prints `LD_LIBRARY_PATH` from the start.

**How it ran.** A driver outside the repo (`build/v5/scripts/v5.sh`) did each run in four steps:

1. A pause of 10 min with Minato closed, the series' rule before a heavy run.
2. The launch and the fetch with `tools/benchmark.sh`'s own commands, into `build/v5/` only. B's launch added
   `-benchmarkGuestEnvironment MINATO_VULKAN_BGRA8_FIRST=1`.
3. A check that the console names format 37 for A and 44 for B. If not, the driver stops.
4. After a dropped link, a new fetch once Minato has ended. An invalid run moves to `build/v5/failed/` and runs again.

Step 4 was never needed. All 7 runs were valid, in portrait, nominal from start to end, and charging at 80 % (apk-53
at 75 %). The order was
native-50, apk-51 and apk-52 on the first build, then apk-53 to apk-56 (B A A B) on the second. The setup was the
series'.

**Unexplained: the A arm varies between launches.**

- apk-51 and apk-52, on the first build, drew 80.37 and 80.47 FPS (a period of 12.44 ms).
- apk-54 and apk-55, on the second build, drew 78.33 and 78.37 FPS (12.77 ms), as the series did (78.23 to 78.43).
- The native control matched the series (86.07 FPS).
- No commit since the series touched the Vulkan path. The first build's `libc.so` bug was in the series' build too,
  and the conditions were the same.

The rule compares the arms within one build and one sitting, so the verdict stands.

**The rest: two suspects.** About 0.48 ms a frame remains, and so do the late frames: 1.4 and 2.7 % of B's intervals
are at 16.67 ms or more, against none of N's. The late frames fit either suspect.

- *`libvulkan`'s queue path* (ADR-0030, V4a2).
  - Every submit signals the queue's timeline semaphore, and waits go in slices of 10 ms.
  - `graphics/host` counts and flags every submit, acquire and present.
  - The GLES APK has none of this.
  - The steady 0.48 ms is on the GPU's side, since B's render thread spends native's CPU time per frame (4.40 and
    4.47 ms against 4.39 ms) [guess, from these numbers].
- *The render thread's QoS:* default in the APK, user-interactive in the native build.
  - B's late frames took more CPU time than its other frames (4.94 and 4.85 ms against 4.40 and 4.47 ms), as a thread
    on a slower core would [guess].
  - The GLES APK has the same QoS and matches its native build, but at a load where both variants show late frames.
- *The cheapest A/B to separate them* (run on 2026-10-01, below): the native build with its render thread at QoS
  default, against user-interactive, in one sitting.
  - It needs a launch argument that `graphics/bench/native/native.m` reads (red first), and about 25 min of the
    iPhone for two runs.
  - If the native build at default loses about 0.48 ms a frame and shows the late frames, QoS is the rest.
  - If it matches user-interactive, the queue path is left, and its A/B comes next.

**The QoS A/B, 2026-10-01: the render thread's QoS is not the rest.** By the rule fixed before the runs, the QoS is
ruled out. At QoS default the native build draws as it does at user-interactive. Its period is 0.012 ms shorter,
within the native runs' spread of about 0.02 ms, its p99 is the same and it shows no late frames. Of the two suspects,
`libvulkan`'s queue path is left. Measured 2026-10-01 from 09:16 to 09:29 on the same iPhone.

| Run | Arm | QoS | FPS | p99 | at 8.33 / 12.50 / 16.67 / 20.83 ms | CPU p50 | Period | Late |
|---|---|---|---:|---:|---|---:|---:|---:|
| native-60 | NU | user-interactive | 86.20 | 12.503 ms | 21.6 / 78.4 / 0.0 / 0.0 % | 4.33 ms | 11.60 ms | 0.0 % |
| native-61 | ND | default | 86.27 | 12.503 ms | 21.9 / 78.1 / 0.0 / 0.0 % | 4.33 ms | 11.59 ms | 0.0 % |

The runs are `vulkan-120-heavy-<run>`, cut as above. Late is the share of intervals at 16.67 ms or more. So the
guess above about B's late frames does not hold: at QoS default the native render thread spends the same CPU time per
frame and draws no late frame.

- *NU:* the native build as in the series and V5, its render thread at user-interactive. The control.
- *ND:* the same build launched with `-benchmarkRenderQoS default`, which `graphics/bench/native/native.m` reads. Its
  render thread runs at QoS default, as the APK's guest render thread does. `run.csv`'s `qos` column, which the layer
  takes from the presenting thread, says so.
- *The rule:* P is the period, L the share of late intervals.
  1. ND's period at least 0.24 ms above NU's (half of the rest) and L at least 0.5 %: QoS explains the rest.
  2. ND's period at most 0.05 ms above NU's and L at most 0.1 %: QoS is ruled out, and the queue path is left.
  3. In between: QoS's share of the rest is (P_ND − P_NU) / 0.48 ms, and two more runs follow.
- *The build:* `tools/benchmark.sh --api vulkan` at 09:06, 281cf00 with the switch (uncommitted). Against V5's second
  build only `MinatoBench` differs. Minato's binary, `libvulkan`, `libc.so` and the APKs are byte-identical.
- *Tests,* red first: `graphics.recorder` runs the GLES host build with `-benchmarkRenderQoS default` and checks that
  the recording notes QoS default. Another value fails the start with its reason. Graphics CTest 28 of 28 in debug
  and release; `MinatoBench` builds for iphoneos and iphonesimulator.
- *How it ran:* `build/v5-qos/scripts/qos.sh` (not in git), as V5's driver: a pause of 10 min with Minato closed
  before each run, then `tools/benchmark.sh`'s own launch and fetch, into `build/v5-qos/` only. It stops if
  `run.csv`'s `qos` does not match the arm. Both runs were valid at the first attempt, in portrait, nominal from
  start to end, and charging at 80 %. The setup was the series'.

**The queue-path A/B, 2026-10-01: `libvulkan`'s queue path is about a third of the rest, and none of the late
frames.** By the rule fixed before the runs this is case 3, between. The native build with `libvulkan`'s queue path in
front of MoltenVK has a period 0.164 ms longer than without it, 0.34 of the rest of 0.48 ms, and shows no late frames.
So about 0.32 ms a frame and all of B's late frames lie outside the queue path. Measured 2026-10-01 from 11:23 to
16:25 on the same iPhone, with a stop while the Mac slept (below).

| Run | Arm | Queue path | FPS | p99 | at 8.33 / 12.50 / 16.67 / 20.83 ms | CPU p50 | Period | Late |
|---|---|---|---:|---:|---|---:|---:|---:|
| native-62 | NU | MoltenVK's | 86.17 | 12.503 ms | 21.5 / 78.5 / 0.0 / 0.0 % | 4.32 ms | 11.606 ms | 0.0 % |
| native-63 | NQ | `libvulkan`'s | 85.07 | 12.503 ms | 18.0 / 82.0 / 0.0 / 0.0 % | 4.32 ms | 11.755 ms | 0.0 % |
| native-64 | NQ | `libvulkan`'s | 85.00 | 12.503 ms | 17.8 / 82.2 / 0.0 / 0.0 % | 4.30 ms | 11.762 ms | 0.0 % |
| native-65 | NU | MoltenVK's | 86.37 | 12.503 ms | 22.1 / 77.9 / 0.0 / 0.0 % | 4.34 ms | 11.582 ms | 0.0 % |

The runs are `vulkan-120-heavy-<run>`, cut as above. NU's period is 11.594 ms (its two runs 0.024 ms apart), NQ's
11.758 ms (0.007 ms apart). In both NQ runs the share of 8.33 ms intervals falls from about 22 % to 18 %: frames that
made the next refresh at 120 Hz now miss it.

- *NU:* the native build as in the series, V5 and the QoS A/B, with MoltenVK's own queue path. The control.
- *NQ:* the same build launched with `-benchmarkQueuePath libvulkan`, which `graphics/bench/native/native.m` reads.
  `graphics/bench/native/queue_path.c` then puts what `libvulkan` changes in what MoltenVK is asked in front of
  MoltenVK (`graphics/vulkan/device.c` and `queue.c`):
  - a device with timeline semaphores and `VK_EXT_metal_objects`, and a timeline semaphore per queue;
  - the queue's timeline signalled last in every submit, at the count of its submits;
  - fence waits and acquires in slices of at most 10 ms.

  The scene's frames stay the same. At the end the run logs its submits and the timeline's value: 4400 submits with the
  timeline at 4400 (nq-63), 4384 at 4384 (nq-64). Its waits took 8822 slices for 8798 waits and 8789 for 8766: two
  waits a frame, of which about 24 a run took more than one slice.
- *The rule* (`build/v5-queue/RULE.md`): P is the period, L the share of late intervals.
  1. P_NQ − P_NU at least 0.24 ms (half of the rest) and L_NQ at least 0.5 %: the queue path explains the rest.
  2. P_NQ − P_NU at most 0.05 ms and L_NQ at most 0.1 %: the queue path is ruled out.
  3. In between: the queue path's share of the rest is (P_NQ − P_NU) / 0.48 ms. Two more runs, nq-64 and nu-65, follow,
     and the rule uses all four.
- *The result:* P_NQ − P_NU = +0.164 ms, a share of 0.34; L_NQ 0.0 %, against B's 1.4 and 2.7 %. The difference is
  about 7 times NU's spread. What is left of the rest lies in how and when the APK asks: the thunks, the runtime's vsync
  channel and glue, the gates. The next step goes to the user.
- *The build:* `tools/benchmark.sh --api vulkan` at 11:12, 0429e7a with the queue path (uncommitted). MoltenVK is
  byte-identical to the QoS A/B's build. `MinatoBench` differs by the queue path. The guest libraries differ by the
  tree's other work since then, but the native arms do not load them.
- *Tests:* `graphics.bench` runs the Vulkan host build at the light load with `-benchmarkQueuePath libvulkan`. The run
  must draw the same frame 1000 as without the argument, log a timeline at the count of its submits (at least one per
  frame), and take a slice or more for each fence wait and acquire (at least two per frame). The run without the
  argument logs no queue path, and another value is refused. Graphics CTest 28 of 28 in debug and release; `MinatoBench`
  builds for iphoneos and iphonesimulator.
- *How it ran:* `build/v5-queue/scripts/queue.sh` (not in git), as the QoS A/B's driver: a pause of 10 min with Minato
  closed before each run, then `tools/benchmark.sh`'s own launch and fetch, into `build/v5-queue/` only. It stops if
  `run.csv`'s `qos` is not user-interactive, if NQ's console lacks the queue path's line with the timeline at its
  submits, or if NU's console names one.
  - nu-62 ran at 11:23. nq-63's first attempt at 11:34 ended 9 s after its launch with the exit code 0. It left an
    empty folder on the iPhone: no scene end, no crash or Jetsam log. Its second attempt, on the same build at 14:00,
    passed, so the first counts as a dropped launch [inferred], not as the queue path's fault. Since then `queue.sh`
    counts such an attempt (the exit code 0 before the scene's end, nothing fetched) as empty. It moves the attempt to
    `failed/` and runs it again after the pause, as it does with an invalid run.
  - nq-64 ran at 14:14. The Mac then slept: its lid closed at 14:19 and it ran on battery from 14:21, so nu-65's pause
    ran only in short wakes. The pause was stopped by hand at 14:59, before anything launched, and nu-65 ran at 16:24
    under `caffeinate -i` with the Mac on power.
  - All four runs were valid at their first attempt that launched, in portrait, nominal from start to end. nu-62 ran
    unplugged at 80 %, the others charging at 65 to 75 %. NU's two runs agree within 0.024 ms all the same.

**The late-frames A/B, 2026-10-01: the APK's layer opacity is the rest.** By the rule fixed before any code this is
case 1: the layer's opacity explains the rest outside the queue path. With its layer opaque, the APK without the copy
draws 98.57 % of the native build's FPS, with its p99 of 12.503 ms and no late frames. Its period is 0.166 ms longer
than native's, the queue path's cost of 0.164 ms. Measured 2026-10-01 from 17:49 to 18:34 on the same iPhone.

| Run | Arm | Layer | FPS | p99 | at 8.33 / 12.50 / 16.67 / 20.83 ms | CPU p50 | Period | Late | To screen | Earliest |
|---|---|---|---:|---:|---|---:|---:|---:|---:|---:|
| native-66 | N | opaque | 86.20 | 12.503 ms | 21.5 / 78.5 / 0.0 / 0.0 % | 4.33 ms | 11.605 ms | 0.0 % | 22.38 ms | −0.23 ms |
| apk-67 | B | not opaque | 83.23 | 16.670 ms | 13.3 / 85.2 / 1.5 / 0.0 % | 4.52 ms | 12.015 ms | 1.5 % | 23.41 ms | −0.95 ms |
| apk-68 | BO | opaque | 84.87 | 12.503 ms | 17.2 / 82.8 / 0.0 / 0.0 % | 4.26 ms | 11.784 ms | 0.0 % | 22.90 ms | −0.24 ms |
| apk-69 | BO | opaque | 85.07 | 12.503 ms | 17.9 / 82.1 / 0.0 / 0.0 % | 4.26 ms | 11.758 ms | 0.0 % | 22.82 ms | −0.24 ms |
| apk-70 | B | not opaque | 83.40 | 16.670 ms | 14.0 / 84.4 / 1.7 / 0.0 % | 4.57 ms | 11.991 ms | 1.7 % | 23.35 ms | −0.94 ms |

The runs are `vulkan-120-heavy-<run>`, cut as above. To screen is the p50 of the time from a frame's `nextDrawable`
to its present. Earliest is the p10 of when a frame took its drawable, counted from frame k−2's present; negative is
before it. B's period is 12.003 ms and BO's 11.771 ms, each pair within 0.026 ms.

- *The data check, before any code* (`build/v5-late/data-check.txt`, not in git), on V5's and the queue A/B's
  `frames.csv`:
  - The APK asks in time. Frame k takes its drawable 0.62 to 0.75 ms (p50) after frame k−2's present in every run,
    native and APK, the earliest it can with 3 drawables. The thunks, the vsync channel and glue, and the gates act
    before that point.
  - The APK loses after the drawable. From the drawable to the screen takes (p50) 22.24 to 22.46 ms native, 22.82
    and 22.89 ms NQ, 23.36 and 23.63 ms B, 24.98 and 24.99 ms A. Half of it plus the 0.7 ms gives each run's period
    within 0.12 ms.
  - The late frames come from the same part. They come in bursts and grow with it: native none, NQ 0.04 and 0 %, B
    1.4 and 2.7 %, A 7.3 and 7.4 %.
  - Every APK run took its earliest drawables 0.98 to 1.15 ms before frame k−2's present, every native run 0.16 to
    0.24 ms. A drawable comes free when the next frame is latched, so the APK's frames took longer from latch to
    screen: another display path.
- *The difference:* the swapchain's `compositeAlpha`.
  - The scene takes OPAQUE where the surface offers it. MoltenVK offers it, so the native build asks for OPAQUE.
    `libvulkan` offers only INHERIT, as Android 15's loader does ("window composition is a WindowManager property").
  - MoltenVK sets `CAMetalLayer.opaque` only for a request other than INHERIT (`MVKSwapchain.mm`, 1.4.2).
  - The session's view, `MetalLayerHostView`, sets a black background color, which in UIKit clears the layer's
    opacity, and its `isOpaque` does not set it back [verified on the Mac's UIKit, and on the iPhone by apk-67's
    note]. So the APK's Vulkan layer stays non-opaque.
  - That Core Animation composites such a layer instead of showing its drawables directly fits the earliest
    drawables [inferred].
  - ANGLE's Metal backend sets no opacity, so both GLES variants draw on the same non-opaque layer, and they pass.
- *N:* the native build as in the series and the A/Bs, its layer opaque. The control.
- *B:* the APK with `MINATO_VULKAN_BGRA8_FIRST=1`, as V5's B: no copy, `libvulkan`'s queue path, INHERIT.
- *BO:* B with `MINATO_VULKAN_OPAQUE=1` as well. `libvulkan` then hands MoltenVK OPAQUE in place of the app's INHERIT
  (`graphics/vulkan/swapchain.c`), so MoltenVK makes the layer opaque. Nothing else changes. Each run's console names
  the layer's opacity at the first drawable, which `graphics/host`'s recorder logs once.
- *The rule* (`build/v5-late/RULE.md`): P is the period, L the share of late intervals, Δq = 0.164 ms the queue
  path's cost.
  1. P_BO − P_N at most Δq + 0.05 ms and L_BO at most 0.1 %: the layer's opacity is the rest.
  2. P_B − P_BO at most 0.05 ms and L_BO at least half of L_B: opacity is ruled out.
  3. In between: opacity's share of the rest is (P_B − P_BO) / (P_B − P_N − Δq).
- *The result:* P_BO − P_N = +0.166 ms and L_BO 0.0 %, so case 1. B is 0.398 ms from N, 0.234 ms of it beyond the
  queue path, and the opaque layer takes 0.232 ms of that. Opaque, the APK's frames go from drawable to screen in
  22.86 ms, as NQ's do, and take their earliest drawables as native's do.
- *The build:* `tools/benchmark.sh --api vulkan` at 17:34, 6db31f7 with the switch and the recorder's note
  (uncommitted). Against the queue A/B's build, `MinatoBench`, MoltenVK and the bench APKs are byte-identical.
  `libvulkan` has the switch, the guest's `liblog` and new `libdl_android` carry Runtime's work, and Minato's binary
  the recorder's note and the tree's other work.
- *Tests,* red first: `graphics.vulkan_window`, where the fake logs alpha 0x1 for the app's INHERIT with
  `MINATO_VULKAN_OPAQUE=1` and 0x8 without. `graphics.recorder`: each recording notes the layer's opacity and the
  console names it once, 0 for the GLES host build and APK, 1 for the Vulkan host build and 0 for the Vulkan APK. The
  tests' layers start non-opaque, as the session's. Graphics CTest 30 of 30 in debug and release, `present_gles`
  skipped.
- *How it ran:* `build/v5-late/scripts/late.sh` (not in git), as the queue A/B's driver, under `caffeinate -i` with
  the Mac on power: a pause of 10 min with Minato closed before each run, then `tools/benchmark.sh`'s own launch and
  fetch, into `build/v5-late/` only. It stops if B's or BO's console lacks format 44 or names another opacity than
  the arm's (N 1, B 0, BO 1). A B with 1 would have left the A/B without a question. The order was N B BO BO B. All
  five runs were valid at their first attempt, in portrait, nominal from start to end, and charging at 80 %.
- *Next* (the rule's case 1): its own step, Minato's window layer opaque by default, as Android 15 makes a
  NativeActivity's window:
  - `NativeActivity.onCreate` sets the window's format to `PixelFormat.RGB_565`, and `ANativeActivity_setWindowFormat`
    reaches `Window.setFormat`.
  - `ViewRootImpl.updateOpacity` makes the surface opaque when the format has no alpha, without surface insets and
    while it is not drag-resizing.
  - The buffers' own format and Vulkan's INHERIT do not change it.

  Then a parity rerun of `vulkan-120-heavy`, and of the GLES configurations, whose layers change too.

The copy's remedies stay ADR-0030's: B8G8R8A8 first through an app profile in M6, or a MoltenVK patch. Either removes
the copy's share. Together with an opaque layer, either gives what BO measured.

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

## ADR-0044: ES 3.1 on ANGLE's Vulkan backend (reported, not judged)

ADR-0044 runs apps that require ES 3.1 on ANGLE's Vulkan backend over our MoltenVK (decision 4, d1). Decision 5 asks
for `bench-gles` on both backends on the iPhone, reported and not judged. d2 made `bench-gles31`: bench-gles's library
under a manifest that requires ES 3.1, so that its session takes the Vulkan path by its manifest, as an ES 3.1 app's
does. It runs as an APK alone, since there is no native ES 3.1 build, beside bench-gles's native runs of the same rate
and load.

**Result: with the light load the Vulkan path holds the rate at 60 and 120 Hz, as the native build does. With the
heavy load at 120 Hz it draws 54.07 FPS against the native build's 96.20 (56.2 %), with a p99 of 20.838 ms against
12.503 ms, where bench-gles on ANGLE-Metal draws 96.07 FPS.** Its render thread needs 6.89 ms of CPU time per frame
against 4.60 ms, well below the 18.5 ms its frames take, so the gap lies in the GPU's work [inferred], which this
benchmark does not split. Measured 2026-10-08 from 09:00 to 11:02 on the iPhone 15 Pro Max, in one build.

| Configuration | FPS: native / Metal / Vulkan | Vulkan / native | p99: native / Metal / Vulkan | Vulkan / native |
|---|---:|---:|---:|---:|
| 60-light | 60.00 / 60.00 / 60.00 | 1.0000 | 16.671 / 16.671 / 16.670 ms | 1.0000 |
| 120-light | 119.97 / 119.97 / 119.97 | 1.0000 | 8.335 / 8.335 / 8.335 ms | 1.0000 |
| 120-heavy | 96.20 / 96.07 / 54.07 | 0.5620 | 12.503 / 12.503 / 20.838 ms | 1.6667 |

- *Native:* `MinatoBench.framework` on ANGLE-Metal, the gles series' native runs. *Metal:* the APK `bench-gles`.
  *Vulkan:* the APK `bench-gles31`.
- Medians by nearest rank over the warm windows (20 s to 50 s after the first present) of 5 valid runs each, as in M3.
  `tools/bench-summary.py` gives the Vulkan rows as "reported", beside gles's native runs.
- bench-gles's own verdicts in this sitting, the Metal APK against the native build: all three pass.

### Where the heavy load goes

- *Intervals in the warm window,* one run each (number 03): the native build 50.6 % at 8.33 ms and 49.4 % at 12.5 ms,
  the Metal APK 50.0 and 50.0 %, the Vulkan APK 56.0 % at 16.67 ms and 44.0 % at 20.83 ms.
- *Steady:* the Vulkan APK's five runs drew 54.03 to 54.30 FPS.
- *CPU time per frame* of the render thread, p50 and p99: native 4.60 and 5.28 ms, Metal APK 4.61 and 5.20 ms, Vulkan
  APK 6.89 and 7.54 ms.
- *Not measured:* which part of the GPU's work costs it. ADR-0044's risks name candidates [guess]: Metal's OpenGL
  mode, which MoltenVK's private API turns on in every render pass of ANGLE's instance; shaders that reach Metal
  through SPIR-V and SPIRV-Cross instead of ANGLE's own MSL; MoltenVK's handling of render passes.

### Follow-up: the GPU's time per frame (reported)

**Result: on the Mac both paths need about the same GPU time per frame. On the iPhone both keep the GPU saturated,
and the Vulkan path's frame period, 18.6 ms against ANGLE-Metal's 10.4 ms, fits more GPU time per frame better than a
stall in presenting or pacing [inferred].** Which part of the GPU's work costs it is still not measured. Measured
2026-10-08, time-boxed, with a scratch hook that adds up the GPU span (`GPUEndTime - GPUStartTime`) of each Metal
command buffer that a guest thread commits. The hook stays out of the tree.

- *The Mac* (Apple M4 Pro, macOS 27, the lead's OK): `bench-gles` at the heavy load through `graphics-app-test`'s
  bench mode, at 1290 × 2796 and 120 Hz, on ANGLE-Metal and on ANGLE's Vulkan backend
  (`MINATO_ANGLE_BACKEND=vulkan`), three runs each of 1,081 presents. Both hold 120 Hz, 8.33 ms between ticks. The
  GPU's time per frame: ANGLE-Metal 5.35, 5.43 and 5.50 ms in one command buffer a frame, ANGLE-Vulkan 5.04, 5.54 and
  6.00 ms in 3.4 to 3.6. The process's CPU time (`time -p`), two runs each: ANGLE-Metal 7.5 and 7.6 s, ANGLE-Vulkan
  5.4 and 5.7 s for the same frames.
- *The iPhone* (11:33 to 11:44, the user's OK): a Release benchmark build of 05c7026 with the hook (run.csv names
  05c7026-gputime), two heavy runs per APK at 120 Hz, all valid, their FPS within 1.2 % of the sitting's medians:

| 120-heavy | FPS | Command buffers a frame | Spans a frame | A span in frame periods | Drawable to present |
|---|---:|---:|---:|---:|---:|
| Metal APK | 96.50 / 96.13 | 1.00 / 0.99 | 14.96 / 14.93 ms | 1.44 / 1.45 | 20.49 / 20.50 ms |
| Vulkan APK | 53.43 / 53.90 | 3.67 / 3.61 | 100.74 / 90.09 ms | 1.47 / 1.34 | 37.15 / 37.14 ms |

- *Spans* of command buffers in flight together overlap, so where the GPU is saturated a frame's spans add up to more
  than its period. On the Mac they add up to less.
- *The same shape:* on both APKs a span lasts about 1.4 frame periods and a drawable is presented 2.0 periods after it
  was taken; only the period differs. The render thread's CPU time p50: 4.59 and 4.61 ms against 6.94 and 6.88 ms, as
  in the sitting.
- *Reading* [inferred]: both APKs keep the iPhone's GPU saturated, and the Vulkan path needs about 18.6 ms of its time
  per frame against 10.4 ms. A stall in presenting or pacing would leave the GPU idle and shorten the spans instead.
  The sums cannot rule out command buffers that wait on events inside their spans.
- *The Mac against the iPhone:* from the Mac to the iPhone, ANGLE-Metal's frame grows 1.9 times (5.4 to 10.4 ms) and
  the Vulkan path's 3.4 times (5.5 to 18.6 ms) [inferred: both GPU-bound on the iPhone]. Both GPUs are of one
  generation (Metal's driver classes AGXG16X and AGXG16P; the M4 Pro is of family Apple9), so a cost in the shaders'
  arithmetic would show on the Mac too. The iPhone has far less memory bandwidth [unverified: about 51 against
  273 GB/s], so the first suspect is the memory traffic per frame [guess]: the render passes' loads and stores,
  storage modes and compression, the tiler's geometry for 8,192 meshes.
- *Not measured yet:* which part of the GPU's work costs it. Next, when there is room: a census of each frame's render
  passes on both paths on the Mac (their load and store actions, storage modes and sample counts), and only if that
  leaves it open, Metal System Trace on the iPhone.

### Cold windows and CPU time (reported)

| Configuration | Side | Cold FPS | Cold p99 | Cold drops | First frame | Warm CPU p50 |
|---|---|---:|---:|---:|---:|---:|
| 60-light | native | 57.42 | 16.671 ms | 22 | 430 ms | 5.89 ms |
| 60-light | Metal APK | 57.19 | 16.671 ms | 2 | 456 ms | 5.91 ms |
| 60-light | Vulkan APK | 57.12 | 16.671 ms | 2 | 469 ms | 6.53 ms |
| 120-light | native | 113.84 | 12.503 ms | 21 | 421 ms | 3.35 ms |
| 120-light | Metal APK | 113.33 | 16.670 ms | 2 | 447 ms | 4.54 ms |
| 120-light | Vulkan APK | 114.04 | 8.335 ms | 1 | 452 ms | 3.67 ms |
| 120-heavy | native | 94.65 | 12.503 ms | 1 | 132 ms | 4.60 ms |
| 120-heavy | Metal APK | 94.56 | 12.503 ms | 1 | 153 ms | 4.61 ms |
| 120-heavy | Vulkan APK | 53.32 | 20.838 ms | 1 | 153 ms | 6.89 ms |

Medians over the valid runs, as M3's table above. The first frame, which ADR-0044 feared for the Vulkan path's shader
translation, comes as soon as on ANGLE-Metal: within 13 ms of bench-gles's at each configuration.

### How it was measured

- *Build:* one Release benchmark build that `tools/benchmark.sh` made from tree `5f0a5008` (ad4281e with d2's 15
  files, the content d2 committed as 119182c). It ran in a clone detached at ad4281e with the tree's files read in, so
  every run's `run.csv` names ad4281e-dirty.
- *Series,* in bench-plan's order (ADR-0044 d2): `tools/benchmark.sh --api gles`, the native build and bench-gles
  interleaved as in M3, then `--api gles31`, bench-gles31 alone with a warm-up and 5 valid runs per configuration.
- *Validity:* decision 6's rules, as in M3. Two gles runs were invalid from heat (thermal_above_fair) and ran again:
  gles-120-heavy-native-01 and gles-120-heavy-apk-05. No gles31 run was invalid.
- *Conditions* of all 50 runs: Release, no debugger, Low Power Mode off, started nominal, the layer 1290 × 2796 in
  `bgra8Unorm` with 3 drawables, the battery charging at 80 %. The thermal state at the end: for gles nominal ×21,
  fair ×11 and serious ×2 (the two invalid runs), for gles31 nominal ×7 and fair ×9. No pauses besides the app's own
  wait for nominal before each run, and no fan.
- *An interruption:* about 10:00 the phone was in use for another app while gles-120-heavy-native-06 waited to draw.
  The run stayed open in the background until `devicectl`'s 900 s timeout ended it at 10:12 without a result. On the
  user's word the series went on at 10:15 where it had stopped; the run was made again and was valid.
- *Order:* the gles31 runs came after the whole gles series, on a phone that had just run the heavy load; each began
  nominal.

### Limits

- One sitting and one build, 5 valid runs per configuration and side.
- The Vulkan rows stand beside native runs made earlier in the same sitting, not interleaved with them.
- bench-gles is an ES 3.0 scene: it uses none of what ES 3.1 adds, such as compute shaders, storage buffers and
  indirect draws.
- The cause of the heavy load's gap is not measured (see above).

### Reproduce

```bash
tools/benchmark.sh --api gles
tools/benchmark.sh --api gles31
python3 tools/bench-summary.py build/benchmark
```

## M4 test 2: Godot (ADR-0042)

Test 2 of M4 measures an engine instead of our own scene. `godot-bench`, a Godot 4.7.2 project of ours, is exported
twice: as Vulkan and GLES APKs, which run in Minato on the Java path (ART in the interpreter, GodotActivity, the APK's
`libgodot_android.so`), and as the twin, Godot's own iOS template inside the benchmark build, which draws with
MoltenVK, with EAGL for GLES, or with Godot's Metal renderer. The harness is ADR-0037's, with ADR-0042's changes for
an engine: the APK's frames are its SurfaceView layer's last drawables, an APK that asks no frame rate is judged by
its frame times (the no-rate rule), and the GLES pair by `bench.gd`'s frame ticks, since EAGL's layer gives the twin
no drawables (ADR-0042).

**Result: parity holds in all six configurations.** The four 120 Hz configurations pass. The two 60 Hz
configurations were "undecided" after 10 runs per variant, because the twin's own p99 spreads more than decision
8's 5 % (26 % with GLES, 33 % with Vulkan); by decision 8's change of 2026-10-29 (a strict majority of the twin's
runs agreeing within 5 % meets the guard) both pass on their medians (below). Measured 2026-10-28 from 17:05 to
20:40 on the iPhone 15 Pro Max (iOS 27), by cable, in one Release build of `40e4a08`, 100 runs, all valid.

| Configuration | Native FPS | APK FPS | FPS ratio | Native p99 | APK p99 | p99 ratio | Native spread FPS / p99 | Runs | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| godot-gles-60-light | 60.00 | 60.00 | 1.0000 | 22.859 ms | 17.667 ms | 0.7729 | 0.11 / 26.02 % | 10 | pass (6 of 10) |
| godot-gles-120-light | 59.97 | 119.97 | 2.0006 | 17.354 ms | 8.916 ms | 0.5138 | 0.06 / 0.52 % | 5 | pass |
| godot-gles-120-heavy | 60.00 | 119.97 | 1.9994 | 17.325 ms | 8.761 ms | 0.5057 | 0.06 / 1.15 % | 5 | pass |
| godot-vulkan-60-light | 60.00 | 60.00 | 1.0000 | 25.005 ms | 25.006 ms | 1.0000 | 0.11 / 33.33 % | 10 | pass (7 of 10) |
| godot-vulkan-120-light | 119.97 | 119.97 | 1.0000 | 8.335 ms | 8.335 ms | 1.0000 | 0.00 / 0.00 % | 5 | pass |
| godot-vulkan-120-heavy | 119.97 | 119.97 | 1.0000 | 8.335 ms | 8.335 ms | 1.0000 | 0.00 / 0.00 % | 5 | pass |

Medians by nearest rank over the warm windows, as in M3; "Runs" are the valid runs per variant. The 60 Hz
configurations got decision 8's 5 more runs per variant; with 10 native runs still spread, the verdict rests on the
twin's runs that agree within 5 % ("6 of 10", "7 of 10"), as decision 8's change of 2026-10-29 says.

- *The 60 Hz p99s fall on ProMotion's steps.* Vulkan: the APK's p99 is 25.006 ms in 10 of 10 runs, the twin's in 7 of
  10 (16.671 ms in the other 3). GLES: the APK's p99 is 17.64 to 17.71 ms in all 10 runs, the twin's 17.44 to
  17.45 ms in 4 and 22.86 to 23.38 ms in 6. So at 60 Hz the APK is as steady as the twin with Vulkan and steadier with
  GLES.
- *The GLES twin draws at 60 Hz in the 120 Hz configurations* (59.97 and 60.00 FPS, p99 17.3 ms), as ADR-0042's test 4
  saw once. That is Godot's own iOS build: its GLES path draws through EAGL's layer [verified: the twin's
  `GDTOpenGLLayer`, ADR-0042 c4.1], while the APK's GLES goes through ANGLE to a `CAMetalLayer` and holds 120 FPS. The
  cause inside Godot's iOS GLES export is not measured. The two GLES 120 Hz rows pass because the APK is faster, not
  because both sides are equal.
- *The heavy load does not load the A17 Pro:* with Godot both sides hold 120 FPS with it, unlike M3's own scene.
- *Rates:* the twin asks 60 or 120 Hz and gets that link period. The APK asks none, as Godot 4.7.2 does with Swappy off
  (ADR-0042, the Swappy gap), and runs on the panel's 120 Hz link, paced by `Engine.max_fps`.

### Metal (reported, not judged)

Godot's own Metal renderer has no APK counterpart; ADR-0042 reports it beside the Vulkan APK's runs.

| Configuration | Metal twin FPS | Vulkan APK FPS | Metal twin p99 | Vulkan APK p99 |
|---|---:|---:|---:|---:|
| godot-metal-60-light | 59.97 | 60.00 | 16.670 ms | 25.006 ms |
| godot-metal-120-light | 119.97 | 119.97 | 8.335 ms | 8.335 ms |
| godot-metal-120-heavy | 119.97 | 119.97 | 8.335 ms | 8.335 ms |

At 60 Hz the Metal twin keeps every frame on 16.67 ms; the Vulkan APK and the Vulkan twin do not (above).

### Cold windows, CPU time and the first frame (reported, not judged)

| Configuration | Variant | Warm p50 | CPU p50 / p99 | Cold FPS | Cold p99 | Cold drops | First frame |
|---|---|---:|---:|---:|---:|---:|---:|
| godot-gles-60-light | native | 16.667 ms | 13.08 / 17.27 ms | 60.20 | 19.852 ms | 0 | n/a |
| godot-gles-60-light | apk | 16.663 ms | 6.12 / 10.29 ms | 60.20 | 17.620 ms | 0 | n/a |
| godot-gles-120-light | native | 16.695 ms | 17.16 / 17.42 ms | 67.20 | 17.461 ms | 0 | n/a |
| godot-gles-120-light | apk | 8.336 ms | 9.12 / 12.60 ms | 118.50 | 9.603 ms | 0 | n/a |
| godot-gles-120-heavy | native | 16.675 ms | 18.01 / 18.65 ms | 67.10 | 17.527 ms | 0 | n/a |
| godot-gles-120-heavy | apk | 8.338 ms | 9.61 / 10.27 ms | 119.00 | 8.843 ms | 0 | n/a |
| godot-vulkan-60-light | native | 16.670 ms | 5.04 / 5.66 ms | 54.19 | 25.006 ms | 0 | 1,125 ms |
| godot-vulkan-60-light | apk | 16.670 ms | 6.86 / 11.77 ms | 51.14 | 25.006 ms | 1 | 1,736 ms |
| godot-vulkan-120-light | native | 8.335 ms | 8.79 / 9.15 ms | 106.90 | 8.335 ms | 0 | 1,122 ms |
| godot-vulkan-120-light | apk | 8.335 ms | 10.00 / 14.06 ms | 101.18 | 8.335 ms | 1 | 1,776 ms |
| godot-vulkan-120-heavy | native | 8.335 ms | 6.91 / 7.96 ms | 113.60 | 8.335 ms | 0 | 475 ms |
| godot-vulkan-120-heavy | apk | 8.335 ms | 9.78 / 17.17 ms | 107.09 | 8.335 ms | 0 | 1,112 ms |
| godot-metal-60-light | native | 16.670 ms | 17.44 / 18.00 ms | 54.55 | 16.671 ms | 0 | 998 ms |
| godot-metal-120-light | native | 8.335 ms | 9.42 / 9.97 ms | 107.97 | 8.335 ms | 0 | 994 ms |
| godot-metal-120-heavy | native | 8.335 ms | 9.75 / 10.23 ms | 114.82 | 8.335 ms | 0 | 337 ms |

Medians over the valid runs. The cold window is a run's first 10 s.

- *CPU* is what `bench.gd` logs per frame: Godot's process and physics time plus the renderer's frame setup. On the
  twin's GLES and Metal paths it is close to the frame period, so it holds a wait there [inferred]; compare it within
  an API only. With Vulkan the APK's main loop takes 1.2 to 2.9 ms more per frame than the twin's at the median, and
  its p99 is higher.
- *First frame:* from the variant's start to its first present. The APK's includes ART's start and GodotActivity's
  Java start in the interpreter; the VM start alone is not split out. The GLES pair is judged on `bench.gd`'s frame
  ticks (`scene.csv`) on both sides, as the twin's CAEAGLLayer gives no drawables (ADR-0042 d4), hence n/a.

### Limits

- One build and one sitting, with the 60 Hz runs added at its end. 34 of 100 runs ended at thermal state "fair", the
  rest "nominal"; none went "serious".
- The verdict compares Godot's iOS export with its Android export in Minato, not Minato with an engine-free baseline.
  Where the twin is slower (GLES at 120 Hz), the row passes without showing Minato's own cost.
- No Swappy: the APK runs with frame pacing off (ADR-0042's known gap), so real Godot 4 APKs, which ship with it on,
  are not covered.
- Decision 8's first spread rule could not decide a configuration whose native p99 jumps between two levels from run
  to run, as both 60 Hz configurations do here; its change of 2026-10-29 decides them by the agreeing majority.
- With Vulkan at 60 Hz the APK's p99 sits on the 25 ms step in all 10 runs, the twin's in 7 (16.67 ms in 3): the
  medians are equal, but the APK never reached the lower step. A follow-up for M6's frame pacing (Swappy, ADR-0042).

### Reproduce

```bash
tools/benchmark.sh --scene godot --api vulkan
tools/benchmark.sh --scene godot --api gles
tools/benchmark.sh --scene godot --api metal
tools/benchmark.sh --scene godot --api vulkan --more godot-vulkan-60-light
tools/benchmark.sh --scene godot --api gles --more godot-gles-60-light
python3 tools/bench-summary.py build/benchmark
```

## M6: FPS unlock (ADR-0066 step 4, by hand)

ADR-0066 lifts an engine's own frame cap per app. Step 4 measures what the unlock gives on the iPhone with M3's scene
(ADR-0037), told by `tools/benchmark.sh --cap 30` to hold an engine cap of 30 fps, as Unity does without a
`targetFrameRate`, and by `--unlock 60|120` to lift it as ADR-0066's wrapper does. Both variants get the same cap and
unlock through `bench.conf`, so each row compares the APK's pacing at that rate with the native build's; run.csv does
not record the cap, so these are by-hand settings, not a parity result. The wrapper itself is checked by
`frame-cap-test` (step 2), whose lines `tools/device-suite.sh` found as expected on the iPhone in the same sitting,
with no unlock and with `--frame-rate 60` and `120`.

**Result: the unlock reaches 60 and 120 FPS at the native build's frame times with GLES, and at 120 FPS with Vulkan.
Vulkan at 30 and 60 FPS is open: the native build's own p99 spreads too much for a verdict. A 10-minute run at 120 FPS
holds its rate; the iPhone warms to "fair", never "serious".** Measured 2026-10-28 from 20:45 to 22:07 on the iPhone
15 Pro Max (iOS 27), in one Release build of `6a36185`, 60 measured runs, all valid, all at thermal state "nominal".

| Configuration | Cap / unlock | Native FPS | APK FPS | FPS ratio | Native p99 | APK p99 | p99 ratio | Native spread FPS / p99 | Verdict |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| gles-120-light | 30 / none | 30.00 | 30.00 | 1.0000 | 41.676 ms | 41.676 ms | 1.0000 | 0.00 / 0.00 % | pass |
| gles-120-light | 30 / 60 | 60.00 | 59.97 | 0.9994 | 25.006 ms | 25.006 ms | 1.0000 | 0.06 / 0.00 % | pass |
| gles-120-light | 30 / 120 | 119.97 | 120.00 | 1.0003 | 8.335 ms | 8.335 ms | 1.0000 | 0.00 / 0.00 % | pass |
| vulkan-120-light | 30 / none | 30.00 | 30.00 | 1.0000 | 37.509 ms | 37.509 ms | 1.0000 | 0.00 / 22.22 % | more runs, open |
| vulkan-120-light | 30 / 60 | 60.00 | 60.00 | 1.0000 | 16.671 ms | 20.838 ms | 1.2500 | 0.06 / 50.00 % | more runs, open |
| vulkan-120-light | 30 / 120 | 119.97 | 120.00 | 1.0003 | 8.335 ms | 8.335 ms | 1.0000 | 0.03 / 0.00 % | pass |

Medians by nearest rank over the warm windows of 5 valid runs per variant, as in M3; no warm window had a dropped
frame. The p50 is the rate's period in every row: 33.341, 16.670 and 8.335 ms.

- *Below 120 FPS the p99 is one or two 120 Hz ticks above the period,* on both sides: with GLES 41.676 ms at 30 FPS
  and 25.006 ms at 60 FPS in every run; with Vulkan 33.3 to 41.7 ms at 30 FPS and 16.7 to 25.0 ms at 60 FPS, varying
  from run to run as much in the native build as in the APK. That is the cap's own pacing on a 120 Hz link, not
  Minato's cost. The two Vulkan rows stay open by decision 8's spread rule and wait for a later sitting's 5 more runs
  per variant.
- *CPU time* (median of the runs' p50): with GLES 5.91 / 5.80 ms (native / APK) at 30, 5.92 / 5.81 ms at 60 and
  3.38 / 4.53 ms at 120 FPS; with Vulkan 4.52 / 4.57, 4.57 / 4.68 and 3.48 / 3.55 ms.

### Ten minutes at 120 FPS (reported)

`gles-120-light`'s APK variant, cap 30 and unlock 120, one run of 600 s (`-benchmarkSeconds 600`), valid. The thermal
state went from "nominal" to "fair" 67 s after the start and stayed there to the end.

| Minute | FPS | p50 | p99 | Late frames | CPU p50 / p99 |
|---:|---:|---:|---:|---:|---:|
| 1 | 119.05 | 8.335 ms | 8.335 ms | 34 | 3.52 / 4.84 ms |
| 2 | 119.92 | 8.335 ms | 8.335 ms | 4 | 4.50 / 4.88 ms |
| 3 | 119.92 | 8.335 ms | 8.335 ms | 3 | 3.66 / 4.84 ms |
| 4 | 119.95 | 8.335 ms | 8.335 ms | 1 | 4.54 / 4.88 ms |
| 5 | 119.90 | 8.335 ms | 8.335 ms | 3 | 3.46 / 4.86 ms |
| 6 | 119.78 | 8.335 ms | 8.335 ms | 10 | 4.56 / 4.90 ms |
| 7 | 119.88 | 8.335 ms | 8.335 ms | 6 | 3.49 / 4.84 ms |
| 8 | 119.77 | 8.335 ms | 8.335 ms | 11 | 3.41 / 4.86 ms |
| 9 | 119.88 | 8.335 ms | 8.335 ms | 5 | 3.43 / 4.85 ms |
| 10 | 119.83 | 8.335 ms | 8.335 ms | 8 | 3.42 / 4.90 ms |

Frame times are the intervals between the frame clock's ticks; a late frame takes more than 1.5 periods (12.5 ms). The
first minute holds the start. The rate does not fall over the ten minutes, so the iPhone does not throttle this load.

### Limits

- M3's light scene stands in for an engine: the Unity test APK is still "Incompatible" in the library (ADR-0020's
  remaining x18 classes), so ADR-0066's sitting with it at "As the App Asks", 60 and 120 waits.
- The heavy load was not measured with an unlock; its discarded first pair at unlock 120 reached thermal state
  "serious" before the light runs.
- One build and one sitting; the heat run is one run of the APK variant.

### Reproduce

```bash
tools/benchmark.sh --api gles --only gles-120-light --cap 30
tools/benchmark.sh --api gles --only gles-120-light --cap 30 --unlock 60
tools/benchmark.sh --api gles --only gles-120-light --cap 30 --unlock 120
tools/benchmark.sh --api vulkan --only vulkan-120-light --cap 30
tools/benchmark.sh --api vulkan --only vulkan-120-light --cap 30 --unlock 60
tools/benchmark.sh --api vulkan --only vulkan-120-light --cap 30 --unlock 120
python3 tools/bench-summary.py build/benchmark-cap30
python3 tools/bench-summary.py build/benchmark-cap30-unlock60
python3 tools/bench-summary.py build/benchmark-cap30-unlock120
tools/device-suite.sh --frame-rate 60
tools/device-suite.sh --frame-rate 120
```

`tools/benchmark.sh` runs series only; the ten-minute run was started as its `launch` does, with
`-runBenchmark gles-120-light-apk-01 -benchmarkSeconds 600 -benchmarkCap 30 -benchmarkUnlock 120`, and its CSVs
fetched as its `fetch` does.
