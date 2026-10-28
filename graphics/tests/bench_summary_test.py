#!/usr/bin/env python3
"""ADR-0037 b): graphics.bench_summary. tools/bench-summary.py on made-up results, run folders as tools/benchmark.sh
fetches them (App's run.csv and frames.csv), in five sets:
- windows: cold from the start to 10 s after the first present, warm from 20 s to 50 s after it, each end excluded;
  FPS, p50 and p99 of the frame intervals by nearest rank, drops by their nextDrawable time, the CPU times;
- validity: each rule of decision 6 broken once, App's own reasons kept, a run that drew the fill load (load_fill),
  present times checked (after their
  drawable, in order; two drawables at one present count once, the earlier as a drop), and the pair rule;
- a series whose configurations all pass at the limits (95 % of the FPS, 105 % of the p99): parity holds;
- the verdicts: a failure by FPS and by p99, the noise rule at its limit, beyond it with 5 runs (more runs) and with
  10 (undecided, the median of 10 by nearest rank), too few valid runs: parity fails;
- bench-gles31's (ADR-0044 d2)), its APK alone: rows beside gles's native runs of the same rate and load, reported
  and not judged, which leave the parity line as it is; and alone, as tools/benchmark.sh --api gles31 runs it.
Warm-up runs (00) are left out. Usage: bench_summary_test.py --tool <bench-summary.py> --scratch <dir>"""
import argparse
import csv
import pathlib
import shutil
import subprocess
import sys

S = 1_000_000_000
MS = 1_000_000
RUN_HEADER = ("run,variant,api,rate,load,meshes,requested_rate,max_fps,link_period_median_ns,thermal_start,"
              "thermal_end,thermal_changes,low_power,battery_state,battery_level,qos,pixel_format,drawable_width,"
              "drawable_height,max_drawable_count,commit,configuration,debugger,focus_losses,start_ns,"
              "first_present_ns,drawables_before_frame0,drawables_after_done,valid,invalid_reasons").split(",")
FRAMES_HEADER = "index,tick_ns,next_drawable_ns,presented_ns,cpu_ns"

failures = []


def fail(what):
    print(f"FAIL: {what}", flush=True)
    failures.append(what)


def write_run(results, run, frames, first, **fields):
    """A run folder: run.csv with valid defaults for the run's rate, `fields` over them, and frames.csv from
    (next_drawable_ns, presented_ns, cpu_ns) in frame order."""
    parts = run.split("-")
    godot = parts[0] == "godot"
    api, rate, load, variant, _ = parts[1:] if godot else parts
    # Test 2's APK has dropped k = 3 boot-image drawables and its one after done; it asks no rate (ADR-0042).
    apk = godot and variant == "apk"
    row = dict(run=run, variant=variant, api=api, rate=rate, load=load, meshes="512", requested_rate=f"{rate}.0",
               drawables_before_frame0="3" if apk else "0", drawables_after_done="1" if apk else "",
               max_fps="120", link_period_median_ns=str(round(S / int(rate))), thermal_start="nominal",
               thermal_end="nominal", thermal_changes="", low_power="0", battery_state="unplugged",
               battery_level="0.8", qos="user-interactive", pixel_format="80", drawable_width="1290",
               drawable_height="2796", max_drawable_count="3", commit="abc1234", configuration="Release",
               debugger="0", focus_losses="", start_ns=str(first - 400 * MS), first_present_ns=str(first), valid="1",
               invalid_reasons="")
    if apk:
        row.update(requested_rate="0.0", link_period_median_ns="8335167")
    row.update({key: str(value) for key, value in fields.items()})
    folder = results / run
    folder.mkdir(parents=True)
    with open(folder / "run.csv", "w", newline="") as file:
        writer = csv.DictWriter(file, RUN_HEADER, lineterminator="\n")
        writer.writeheader()
        writer.writerow(row)
    lines = [FRAMES_HEADER] + [f"{index},{taken - MS},{taken},{presented},{cpu}"
                               for index, (taken, presented, cpu) in enumerate(frames)]
    if godot and api == "gles":
        # The script's frames: ticks count from Godot's start, so only their intervals mean something.
        ticks = ["index,tick_ns,cpu_ns"] + [f"{index},{presented - first + 5 * S},{cpu}"
                                            for index, (_, presented, cpu) in enumerate(frames)]
        (folder / "scene.csv").write_text("\n".join(ticks) + "\n")
        if variant == "twin":
            lines = lines[:1]
    (folder / "frames.csv").write_text("\n".join(lines) + "\n")


def presents(first, times, cpu=5 * MS):
    """Frames presented at `first` + each time, taken 2 ms before."""
    return [(first + time - 2 * MS, first + time, cpu) for time in times]


def warm_times(frames, p99_ns, longs=30):
    """`frames` present times within the warm window's 30 s, from its start: short intervals, then `longs`
    intervals of `p99_ns`, which are then the p99 by nearest rank."""
    short = (30 * S - 1 - longs * p99_ns) // (frames - 1 - longs)
    times, time = [0], 0
    for index in range(frames - 1):
        time += short if index < frames - 1 - longs else p99_ns
        times.append(time)
    assert times[-1] < 30 * S
    return times


def standard(first, fps, p99_ms, cpu=5 * MS):
    """52 s of frames: every 10 ms before the warm window and after it, fps × 30 frames in it."""
    times = [k * 10 * MS for k in range(2000)]
    times += [20 * S + time for time in warm_times(round(fps * 30), round(p99_ms * MS))]
    times += [50 * S + k * 10 * MS for k in range(201)]
    return presents(first, times, cpu)


def summarize(tool, results, out):
    result = subprocess.run([sys.executable, str(tool), str(results), "--out", str(out)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=300)
    if result.returncode != 0:
        fail(f"{results.name}: bench-summary.py ended with {result.returncode}:\n{result.stdout[-2000:]}")
        return None
    tables = {}
    for name in ("cold", "warm", "verdict"):
        try:
            with open(out / f"{name}.csv", newline="") as file:
                rows = list(csv.DictReader(file))
        except OSError as error:
            fail(f"{results.name}: no {name}.csv ({error})")
            return None
        key = "configuration" if name == "verdict" else "run"
        tables[name] = {row[key]: row for row in rows}
    parity = [line for line in result.stdout.splitlines() if line.startswith("parity: ")]
    tables["parity"] = parity[-1] if parity else "no parity line"
    godot = [line for line in result.stdout.splitlines() if line.startswith("godot parity: ")]
    tables["godot"] = godot[-1] if godot else "no godot parity line"
    return tables


def expect(tables, table, key, field, wanted, what):
    row = tables[table].get(key)
    if row is None:
        fail(f"{what}: no row {key} in {table}.csv")
        return
    if row.get(field) != wanted:
        fail(f"{what}: {table}.csv {key} {field} is {row.get(field)!r}, not {wanted!r}")


def check_windows(tool, scratch):
    results = scratch / "windows"
    first = 1000 * S
    # Every 20 ms for 20 s, then every 10 ms up to 52 s; drops at 5, 30, 40 and 51 s, the one at 30 s costlier.
    times = [k * 20 * MS for k in range(1000)] + [20 * S + k * 10 * MS for k in range(3201)]
    frames = presents(first, times)
    for at, cpu in ((5 * S, 9 * MS), (30 * S + 5 * MS, 9 * MS), (40 * S + 5 * MS, 9 * MS), (51 * S + 5 * MS, 9 * MS)):
        frames.append((first + at, 0, cpu))
    frames.sort()
    write_run(results, "gles-120-light-native-01", frames, first)
    # Sparse in the warm window: 11 presents with the intervals 100 to 1000 ms, and CPU times of 1 to 11 ms.
    sparse = [k * 10 * MS for k in range(2000)]
    warm = [0, 100, 300, 600, 1000, 1500, 2100, 2800, 3600, 4500, 5500]
    frames = presents(first, sparse) + [(first + 20 * S + at * MS - 2 * MS, first + 20 * S + at * MS, (k + 1) * MS)
                                        for k, at in enumerate(warm)]
    frames += presents(first, [50 * S + k * 10 * MS for k in range(201)])
    write_run(results, "gles-120-light-apk-01", frames, first)
    tables = summarize(tool, results, scratch / "windows-summary")
    if tables is None:
        return
    for field, wanted in (("window_s", "10.400"), ("frames", "500"), ("fps", "48.08"), ("p50_ms", "20.000"),
                          ("p99_ms", "20.000"), ("drops", "1"), ("first_frame_ms", "400.000"), ("valid", "1")):
        expect(tables, "cold", "gles-120-light-native-01", field, wanted, "the cold window")
    for field, wanted in (("window_s", "30.000"), ("frames", "3000"), ("fps", "100.00"), ("p50_ms", "10.000"),
                          ("p99_ms", "10.000"), ("drops", "2"), ("cpu_p50_ms", "5.000"), ("cpu_p99_ms", "5.000")):
        expect(tables, "warm", "gles-120-light-native-01", field, wanted, "the warm window")
    for field, wanted in (("frames", "11"), ("fps", "0.37"), ("p50_ms", "500.000"), ("p99_ms", "1000.000"),
                          ("cpu_p50_ms", "6.000"), ("cpu_p99_ms", "11.000"), ("drops", "0")):
        expect(tables, "warm", "gles-120-light-apk-01", field, wanted, "nearest rank")
    expect(tables, "verdict", "gles-120-light", "verdict", "incomplete", "one run per variant")
    if tables["parity"] != "parity: not decided":
        fail(f"one configuration's runs: {tables['parity']}")


def check_validity(tool, scratch):
    results = scratch / "validity"
    broken = {
        "01": ({"low_power": 1}, None, "low_power"),
        "02": ({"thermal_start": "fair"}, None, "thermal_start"),
        "03": ({"thermal_changes": "1000=fair|2000=serious", "thermal_end": "serious"}, None, "thermal_above_fair"),
        "04": ({"link_period_median_ns": 16_900_000}, None, "link_period"),
        "05": ({"link_period_median_ns": 16_800_000}, None, ""),
        "06": ({"debugger": 1}, None, "debugger"),
        "07": ({"focus_losses": "123456"}, None, "focus_loss"),
        "08": ({"valid": 0, "invalid_reasons": "frames 10, drawables 11"}, None, "frames 10, drawables 11"),
        "09": ({}, "before", "present_before_drawable"),
        "10": ({}, "order", "presents_out_of_order"),
        "11": ({}, "none", "no_present"),
        "12": ({}, "short", "short_run"),
        "13": ({"thermal_changes": "1000=fair", "thermal_end": "fair"}, None, ""),
        "14": ({"link_period_median_ns": 0}, None, "link_period"),
        "15": ({}, "shared", ""),
        # M6's MetalFX plan, step 0: a run that drew the fill load is not its configuration's.
        "16": ({"load": "fill"}, None, "load_fill"),
    }
    for number, (fields, frames_change, _) in broken.items():
        first = (1000 + int(number) * 100) * S
        frames = standard(first, 100, 12)
        if frames_change == "before":
            taken, _, cpu = frames[2500]
            frames[2500] = (taken, taken - MS, cpu)
        elif frames_change == "order":
            # Each after its own drawable, the second before the first.
            (a_taken, a_presented, a_cpu), (_, b_presented, b_cpu) = frames[2500], frames[2501]
            frames[2500], frames[2501] = (a_taken, b_presented, a_cpu), (a_presented - 3 * MS, a_presented, b_cpu)
        elif frames_change == "shared":
            # As the APKs' first two drawables on the iPhone: both taken before the first present, and both
            # reaching the screen at it. One presented frame; drawable 0 was superseded at that refresh, a drop.
            cpu = frames[0][2]
            frames[0], frames[1] = (first - 12 * MS, first, cpu), (first - 5 * MS, first, cpu)
        elif frames_change == "none":
            frames = [(taken, 0, cpu) for taken, _, cpu in frames]
        elif frames_change == "short":
            frames = [frame for frame in frames if frame[1] < first + 40 * S]
        write_run(results, f"vulkan-60-light-native-{number}", frames, first, **fields)
    # The APK's runs are valid but hold 2 drawables, not 3: the pair differs.
    for number in range(1, 6):
        first = (3000 + number * 100) * S
        write_run(results, f"vulkan-60-light-apk-{number:02}", standard(first, 95, 12), first, max_drawable_count=2)
    # An APK that asks for no rate (requested_rate 0) runs at the panel's rate, so its link period is no reason
    # (ADR-0042); with a rate asked, or as the native twin, the same row is invalid.
    for name, fields in (("16", dict(requested_rate="0.0")), ("17", dict(requested_rate="60.0"))):
        first = (1000 + int(name) * 100) * S
        write_run(results, f"vulkan-60-light-apk-{name}", standard(first, 100, 12), first,
                  link_period_median_ns=8_335_167, **fields)
    first = 1800 * S
    write_run(results, "vulkan-60-light-native-18", standard(first, 100, 12), first, requested_rate="0.0",
              link_period_median_ns=8_335_167)
    tables = summarize(tool, results, scratch / "validity-summary")
    if tables is None:
        return
    for run, reasons in (("vulkan-60-light-apk-16", ""), ("vulkan-60-light-apk-17", "link_period"),
                         ("vulkan-60-light-native-18", "link_period")):
        expect(tables, "warm", run, "invalid_reasons", reasons, f"the link period of {run}")
    for number, (_, _, reasons) in broken.items():
        run = f"vulkan-60-light-native-{number}"
        expect(tables, "warm", run, "valid", "0" if reasons else "1", f"validity of {run}")
        expect(tables, "warm", run, "invalid_reasons", reasons, f"validity of {run}")
    # The cold window of 15: presents at 0 (shared), 20, 30 ... 9990 ms, not 0, 10, 20 ... 9990.
    for field, wanted in (("frames", "999"), ("drops", "1")):
        expect(tables, "cold", "vulkan-60-light-native-15", field, wanted, "a present shared by two drawables")
    expect(tables, "verdict", "vulkan-60-light", "verdict", "pair differs", "the pair rule")


def configuration(results, runs, name, native, apk):
    """The runs of a configuration: `native` and `apk` list (fps, p99 in ms) per run; `runs` counts all runs."""
    for variant, values in (("native", native), ("apk", apk)):
        for number, (fps, p99) in enumerate(values, start=1):
            runs[0] += 1
            first = (1000 + runs[0] * 100) * S
            write_run(results, f"{name}-{variant}-{number:02}", standard(first, fps, p99), first)


def check_parity(tool, scratch):
    results = scratch / "parity"
    runs = [0]
    for api in ("gles", "vulkan"):
        for config in (f"{api}-60-light", f"{api}-120-light", f"{api}-120-heavy"):
            configuration(results, runs, config, [(100, 12)] * 5, [(95, 12.6)] * 5)
        # The warm-up pair, which would fail: left out.
        for variant in ("native", "apk"):
            first = (9000 + len(api) * 100) * S + (variant == "apk") * 100 * S
            write_run(results, f"{api}-120-heavy-{variant}-00", standard(first, 50, 40), first)
    tables = summarize(tool, results, scratch / "parity-summary")
    if tables is None:
        return
    for api in ("gles", "vulkan"):
        for config in (f"{api}-60-light", f"{api}-120-light", f"{api}-120-heavy"):
            for field, wanted in (("verdict", "pass"), ("native_runs", "5"), ("apk_runs", "5"),
                                  ("fps_ratio", "0.9500"), ("p99_ratio", "1.0500"), ("native_fps", "100.00"),
                                  ("apk_p99_ms", "12.600")):
                expect(tables, "verdict", config, field, wanted, "the limits")
        if f"{api}-120-heavy-native-00" in tables["warm"]:
            fail(f"the warm-up run {api}-120-heavy-native-00 is in warm.csv")
    if tables["parity"] != "parity: holds":
        fail(f"a series that passes at the limits: {tables['parity']}")


def check_verdicts(tool, scratch):
    results = scratch / "verdicts"
    runs = [0]
    configuration(results, runs, "gles-120-heavy", [(100, 12)] * 5, [(2849 / 30, 12)] * 5)
    configuration(results, runs, "gles-120-light", [(100, 12)] * 5, [(96, 12.61)] * 5)
    configuration(results, runs, "gles-60-light", [(100, 12)] * 4 + [(95, 12)], [(95, 12)] * 5)
    configuration(results, runs, "vulkan-120-heavy", [(100, 12)] * 4 + [(100, 12.72)], [(100, 12)] * 5)
    configuration(results, runs, "vulkan-120-light",
                  [(fps, 12) for fps in (94, 95, 96, 97, 98, 99, 100, 100, 100, 100)], [(100, 12)] * 10)
    configuration(results, runs, "vulkan-60-light", [(100, 12)] * 5, [(100, 12)] * 4)
    tables = summarize(tool, results, scratch / "verdicts-summary")
    if tables is None:
        return
    for config, field, wanted in (
            ("gles-120-heavy", "verdict", "fail"), ("gles-120-heavy", "fps_ratio", "0.9497"),
            ("gles-120-light", "verdict", "fail"), ("gles-120-light", "p99_ratio", "1.0508"),
            ("gles-60-light", "verdict", "pass"), ("gles-60-light", "native_fps_spread", "5.00"),
            ("vulkan-120-heavy", "verdict", "more runs"), ("vulkan-120-heavy", "native_p99_spread", "6.00"),
            ("vulkan-120-light", "verdict", "undecided"), ("vulkan-120-light", "native_fps", "98.00"),
            ("vulkan-120-light", "native_fps_spread", "6.12"), ("vulkan-60-light", "verdict", "incomplete")):
        expect(tables, "verdict", config, field, wanted, "the verdicts")
    if tables["parity"] != "parity: fails":
        fail(f"a series with failures: {tables['parity']}")


def check_es31(tool, scratch):
    results = scratch / "es31"
    runs = [0]
    for api in ("gles", "vulkan"):
        for config in (f"{api}-60-light", f"{api}-120-light", f"{api}-120-heavy"):
            configuration(results, runs, config, [(100, 12)] * 5, [(95, 12.6)] * 5)
    # Below the limits at 60 Hz, which would fail a judged configuration, and at them at 120 Hz.
    configuration(results, runs, "gles31-60-light", [], [(80, 15)] * 5)
    configuration(results, runs, "gles31-120-light", [], [(95, 12.6)] * 5)
    tables = summarize(tool, results, scratch / "es31-summary")
    if tables is None:
        return
    for config, field, wanted in (
            ("gles31-60-light", "verdict", "reported"), ("gles31-60-light", "native_runs", "5"),
            ("gles31-60-light", "apk_runs", "5"), ("gles31-60-light", "native_fps", "100.00"),
            ("gles31-60-light", "apk_fps", "80.00"), ("gles31-60-light", "fps_ratio", "0.8000"),
            ("gles31-120-light", "verdict", "reported"), ("gles31-120-light", "p99_ratio", "1.0500")):
        expect(tables, "verdict", config, field, wanted, "bench-gles31")
    if tables["parity"] != "parity: holds":
        fail(f"bench-gles31's rows beside a series that passes: {tables['parity']}")
    alone = scratch / "es31-alone"
    configuration(alone, [0], "gles31-120-heavy", [], [(100, 12)] * 5)
    tables = summarize(tool, alone, scratch / "es31-alone-summary")
    if tables is None:
        return
    for field, wanted in (("verdict", "reported"), ("native_runs", "0"), ("native_fps", ""), ("apk_fps", "100.00")):
        expect(tables, "verdict", "gles31-120-heavy", field, wanted, "bench-gles31 alone")


def godot_configuration(results, runs, name, twin, apk, fields=None):
    """Test 2's runs of a configuration `godot-<api>-<rate>-<load>`: `twin` and `apk` list (fps, p99 in ms) per run;
    `fields` maps (variant, number) to the run.csv fields that differ."""
    for variant, values in (("twin", twin), ("apk", apk)):
        for number, (fps, p99) in enumerate(values, start=1):
            runs[0] += 1
            first = (1000 + runs[0] * 100) * S
            write_run(results, f"{name}-{variant}-{number:02}", standard(first, fps, p99), first,
                      **(fields or {}).get((variant, number), {}))


def check_godot(tool, scratch):
    results = scratch / "godot"
    runs = [0]
    for config in ("godot-gles-60-light", "godot-gles-120-light", "godot-gles-120-heavy", "godot-vulkan-60-light",
                   "godot-vulkan-120-heavy"):
        godot_configuration(results, runs, config, [(100, 12)] * 5, [(95, 12.6)] * 5)
    # k and the drawable after done: each run must hold decision d4's, and the series the same k in every run.
    godot_configuration(results, runs, "godot-vulkan-120-light", [(100, 12)] * 5, [(95, 12.6)] * 5, {
        ("apk", 1): dict(drawables_before_frame0=11), ("apk", 2): dict(drawables_before_frame0=-1),
        ("apk", 3): dict(drawables_after_done=2), ("apk", 4): dict(drawables_after_done=""),
        ("apk", 5): dict(drawables_before_frame0=4)})
    # A series whose fifth run drifts from the others' k.
    godot_configuration(results, runs, "godot-metal-120-heavy", [(100, 12)] * 5, [])
    # A drift alone: four runs with k = 3 and one with 4.
    drift = scratch / "godot-drift"
    for config in ("godot-gles-60-light", "godot-gles-120-light", "godot-gles-120-heavy", "godot-vulkan-60-light",
                   "godot-vulkan-120-light", "godot-vulkan-120-heavy"):
        godot_configuration(drift, runs, config, [(100, 12)] * 5, [(95, 12.6)] * 5,
                            {("apk", 5): dict(drawables_before_frame0=4)} if config == "godot-vulkan-60-light"
                            else None)
    tables = summarize(tool, results, scratch / "godot-summary")
    if tables is not None:
        for run, reasons in (("godot-vulkan-120-light-apk-01", "drawables_before_frame0"),
                             ("godot-vulkan-120-light-apk-02", "drawables_before_frame0"),
                             ("godot-vulkan-120-light-apk-03", "drawables_after_done"),
                             ("godot-vulkan-120-light-apk-04", "drawables_after_done"),
                             ("godot-gles-60-light-apk-01", ""), ("godot-gles-60-light-twin-01", ""),
                             ("godot-vulkan-120-light-twin-01", "")):
            expect(tables, "warm", run, "invalid_reasons", reasons, f"test 2's validity of {run}")
        for field, wanted in (("verdict", "pass"), ("native_runs", "5"), ("apk_runs", "5"), ("fps_ratio", "0.9500"),
                              ("p99_ratio", "1.0500")):
            expect(tables, "verdict", "godot-gles-60-light", field, wanted, "the twin against the APK")
        expect(tables, "verdict", "godot-vulkan-120-light", "verdict", "incomplete", "runs that break d4")
        expect(tables, "warm", "godot-vulkan-120-light-twin-01", "variant", "twin", "the twin's variant")
        # Metal has a twin only: reported beside the Vulkan APK of its rate and load, never judged.
        for field, wanted in (("verdict", "reported"), ("native_runs", "5"), ("apk_runs", "5"),
                              ("fps_ratio", "0.9500")):
            expect(tables, "verdict", "godot-metal-120-heavy", field, wanted, "metal's twin beside the Vulkan APK")
        # GLES has no drawables on the twin: frames.csv holds the header alone, the script's scene.csv has the
        # frames, and the judged numbers of both variants are its tick intervals (ADR-0042 d4), the APK's presents
        # reported beside them. The pair rule compares no layer there.
        expect(tables, "warm", "godot-gles-60-light-twin-01", "fps", "100.00", "the GLES twin's ticks")
        expect(tables, "warm", "godot-gles-60-light-apk-01", "p99_ms", "12.600", "the GLES APK's ticks")
        expect(tables, "warm", "godot-gles-60-light-apk-01", "presented_p99_ms", "12.600", "the APK's presents")
        expect(tables, "warm", "godot-gles-60-light-twin-01", "presented_p99_ms", "", "no presents of the twin")
        if tables["godot"] != "godot parity: not decided":
            fail(f"test 2's series with a configuration that is incomplete: {tables['godot']}")
        if tables["parity"] != "parity: not decided":
            fail(f"ADR-0037's line beside test 2's runs alone: {tables['parity']}")
    tables = summarize(tool, drift, scratch / "godot-drift-summary")
    if tables is not None:
        expect(tables, "warm", "godot-vulkan-60-light-apk-05", "invalid_reasons", "drawables_before_frame0 drifts",
               "k that drifts from the series' majority")
        expect(tables, "warm", "godot-vulkan-60-light-apk-04", "invalid_reasons", "", "k of the majority")
        expect(tables, "verdict", "godot-vulkan-60-light", "verdict", "incomplete", "a series with a drift")
        if tables["godot"] != "godot parity: not decided":
            fail(f"test 2's series with a drift: {tables['godot']}")
    clean = scratch / "godot-clean"
    for config in ("godot-gles-60-light", "godot-gles-120-light", "godot-gles-120-heavy", "godot-vulkan-60-light",
                   "godot-vulkan-120-light", "godot-vulkan-120-heavy"):
        godot_configuration(clean, runs, config, [(100, 12)] * 5, [(95, 12.6)] * 5)
    tables = summarize(tool, clean, scratch / "godot-clean-summary")
    if tables is not None and tables["godot"] != "godot parity: holds":
        fail(f"test 2's series that passes: {tables['godot']}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--scratch", required=True)
    args = parser.parse_args()
    tool, scratch = pathlib.Path(args.tool), pathlib.Path(args.scratch)
    shutil.rmtree(scratch, ignore_errors=True)
    scratch.mkdir(parents=True)
    if not tool.exists():
        fail(f"no {tool}")
    else:
        for check in (check_windows, check_validity, check_parity, check_verdicts, check_es31, check_godot):
            check(tool, scratch)
    print(f"{len(failures)} failures" if failures else "PASS: graphics.bench_summary")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
