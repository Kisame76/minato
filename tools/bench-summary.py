#!/usr/bin/env python3
"""The parity benchmark's summary (ADR-0037, decisions 4 to 8). Reads the run folders that tools/benchmark.sh fetched
(<results>/<run>/run.csv and frames.csv, App's, with the scene's scene.csv joined into frames.csv) and writes into
<out>, by default <results>/summary:
- cold.csv and warm.csv, a row per run: cold from the variant's start to 10 s after its first present, warm from
  20 s to 50 s after it, by present time, each end excluded. FPS is the presents in the window over its length; the
  frame times are the intervals between two presents in it, with p50 and p99 by nearest rank; drops are drawables
  never presented (presented_ns 0) whose nextDrawable fell in the window; the CPU times are the scene's for the
  frames in it. Warm-up runs (number 00) are left out.
- verdict.csv, a row per configuration, on the warm windows of its valid runs. A run is valid when decision 6's
  rules hold (recomputed from run.csv, and App's own reasons kept) and its present times are plausible: each after
  its drawable was taken, in frame order, up to the warm window's end. The pair rule: the valid runs of both
  variants have the same drawable size and count, else "pair differs". With fewer than 5 valid runs per variant,
  "incomplete". The noise rule: the native runs' FPS and p99 each spread at most 5 % (max - min over the median),
  else "more runs" (5 more per variant) or, from 10 native runs on, "undecided". Then "pass" when the APK's median
  FPS is at least 95 % of the native's and its median p99 at most 105 %, else "fail". Medians by nearest rank.
It prints the verdicts and then "parity: holds" (every configuration of decision 7 passed), "parity: fails" (one
failed) or "parity: not decided".
Usage: tools/bench-summary.py [<results>, default build/benchmark] [--out <folder>]"""
import argparse
import csv
import math
import pathlib
import re
import sys
from fractions import Fraction

S = 1_000_000_000
COLD_S, WARM_BEGIN_S, WARM_END_S = 10, 20, 50
SERIES = [f"{api}-{rate}-{load}" for api in ("gles", "vulkan")
          for rate, load in ((60, "light"), (120, "light"), (120, "heavy"))]
VALID_RUNS = 5
NOISE = Fraction(5, 100)
FPS_LEAST = Fraction(95, 100)
P99_MOST = Fraction(105, 100)
THERMAL = ["nominal", "fair", "serious", "critical"]
RUN = re.compile(r"((?:gles|vulkan)-[0-9]+-(?:light|heavy))-(native|apk)-([0-9]{2})")
FRAMES_HEADER = ["index", "tick_ns", "next_drawable_ns", "presented_ns", "cpu_ns"]
WINDOW_COLUMNS = ["run", "configuration", "variant", "valid", "invalid_reasons", "window_s", "frames", "fps",
                  "p50_ms", "p99_ms", "drops", "cpu_p50_ms", "cpu_p99_ms", "first_frame_ms"]
VERDICT_COLUMNS = ["configuration", "native_runs", "apk_runs", "native_fps", "apk_fps", "fps_ratio", "native_p99_ms",
                   "apk_p99_ms", "p99_ratio", "native_fps_spread", "native_p99_spread", "native_cpu_p50_ms",
                   "apk_cpu_p50_ms", "verdict", "reason"]


def nearest_rank(values, percent):
    """The value of rank ceil(percent / 100 * n) among the sorted values, None without values."""
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(1, math.ceil(Fraction(percent, 100) * len(ordered))) - 1]


def spread(values):
    """(max - min) over the median."""
    median = nearest_rank(values, 50)
    return Fraction(max(values) - min(values)) / median if median else Fraction(0)


def number(value, digits):
    return "" if value is None else f"{float(value):.{digits}f}"


def ms(value):
    return number(None if value is None else Fraction(value, 1_000_000), 3)


class Run:
    def __init__(self, folder):
        with open(folder / "run.csv", newline="") as file:
            rows = list(csv.DictReader(file))
        if len(rows) != 1:
            raise ValueError(f"{folder}/run.csv holds {len(rows)} runs")
        self.row = rows[0]
        match = RUN.fullmatch(self.row["run"])
        if match is None:
            raise ValueError(f"{folder}/run.csv names the run {self.row['run']!r}")
        self.name = self.row["run"]
        self.configuration, self.variant, self.number = match.group(1), match.group(2), int(match.group(3))
        self.layer = f"{self.row['drawable_width']}x{self.row['drawable_height']}/{self.row['max_drawable_count']}"
        self.frames, self.reasons = [], []
        try:
            with open(folder / "frames.csv", newline="") as file:
                rows = list(csv.reader(file))
            if not rows or rows[0] != FRAMES_HEADER:
                raise ValueError("no header")
            self.frames = [tuple(int(field) for field in row) for row in rows[1:]]
            if any(len(frame) != 5 or frame[0] != index for index, frame in enumerate(self.frames)):
                raise ValueError("the indices are not 0 to n - 1")
        except (OSError, ValueError) as error:
            self.frames = []
            self.reasons.append(f"frames_csv ({error})")
        presents = [frame[3] for frame in self.frames if frame[3] != 0]
        self.first = min(presents) if presents else None
        self.check(presents)
        self.cold = self.window("cold")
        self.warm = self.window("warm")
        if self.first is not None and self.warm["frames"] < 2 and "short_run" not in self.reasons:
            self.reasons.append("warm_window_empty")

    def check(self, presents):
        """Decision 6's rules and the frames' plausibility, then App's own reasons."""
        row, own = self.row, []
        if row["low_power"] == "1":
            own.append("low_power")
        if row["thermal_start"] != "nominal":
            own.append("thermal_start")
        states = [row["thermal_start"]] + [change.split("=", 1)[-1] for change in row["thermal_changes"].split("|")
                                           if change]
        if any(state not in THERMAL[:2] for state in states):
            own.append("thermal_above_fair")
        period = Fraction(S, int(row["rate"]))
        link = int(row["link_period_median_ns"] or 0)
        if link <= 0 or abs(link - period) > period / 100:
            own.append("link_period")
        if row["debugger"] == "1":
            own.append("debugger")
        if row["focus_losses"]:
            own.append("focus_loss")
        if self.frames and self.first is None:
            own.append("no_present")
        elif self.frames:
            if any(presented != 0 and presented <= taken for _, _, taken, presented, _ in self.frames):
                own.append("present_before_drawable")
            if any(later <= earlier for earlier, later in zip(presents, presents[1:])):
                own.append("presents_out_of_order")
            if max(frame[2] for frame in self.frames) < self.first + WARM_END_S * S:
                own.append("short_run")
        if row["valid"] != "1":
            own += [reason for reason in row["invalid_reasons"].split("|") if reason] or ["invalid in run.csv"]
        for reason in own:
            if reason not in self.reasons:
                self.reasons.append(reason)

    @property
    def valid(self):
        return not self.reasons

    def window(self, which):
        numbers = dict(window_s=None, frames=0, fps=None, p50=None, p99=None, drops=0, cpu_p50=None, cpu_p99=None)
        if self.first is None:
            return numbers
        if which == "cold":
            start = int(self.row["start_ns"] or 0)
            begin, end = (start if 0 < start <= self.first else self.first), self.first + COLD_S * S
        else:
            begin, end = self.first + WARM_BEGIN_S * S, self.first + WARM_END_S * S
        inside = [frame for frame in self.frames
                  if (frame[3] != 0 and begin <= frame[3] < end) or (frame[3] == 0 and begin <= frame[2] < end)]
        times = sorted(frame[3] for frame in inside if frame[3] != 0)
        intervals = [later - earlier for earlier, later in zip(times, times[1:])]
        cpu = [frame[4] for frame in inside]
        seconds = Fraction(end - begin, S)
        numbers.update(window_s=seconds, frames=len(times), fps=len(times) / seconds,
                       p50=nearest_rank(intervals, 50), p99=nearest_rank(intervals, 99),
                       drops=sum(1 for frame in inside if frame[3] == 0), cpu_p50=nearest_rank(cpu, 50),
                       cpu_p99=nearest_rank(cpu, 99))
        return numbers

    def window_row(self, which):
        numbers = self.cold if which == "cold" else self.warm
        first_frame = None if self.first is None else self.first - int(self.row["start_ns"] or 0)
        return dict(run=self.name, configuration=self.configuration, variant=self.variant,
                    valid="1" if self.valid else "0", invalid_reasons="|".join(self.reasons),
                    window_s=number(numbers["window_s"], 3),
                    frames=str(numbers["frames"]), fps=number(numbers["fps"], 2), p50_ms=ms(numbers["p50"]),
                    p99_ms=ms(numbers["p99"]), drops=str(numbers["drops"]), cpu_p50_ms=ms(numbers["cpu_p50"]),
                    cpu_p99_ms=ms(numbers["cpu_p99"]), first_frame_ms=ms(first_frame))


def verdict(configuration, runs):
    valid = {variant: [run for run in runs if run.variant == variant and run.valid] for variant in ("native", "apk")}
    fps = {variant: [run.warm["fps"] for run in valid[variant]] for variant in valid}
    p99 = {variant: [run.warm["p99"] for run in valid[variant]] for variant in valid}
    cpu = {variant: [run.warm["cpu_p50"] for run in valid[variant]] for variant in valid}
    median = {variant: (nearest_rank(fps[variant], 50), nearest_rank(p99[variant], 50)) for variant in valid}
    native_fps, native_p99 = median["native"]
    apk_fps, apk_p99 = median["apk"]
    fps_ratio = apk_fps / native_fps if apk_fps is not None and native_fps else None
    p99_ratio = Fraction(apk_p99, native_p99) if apk_p99 is not None and native_p99 else None
    fps_spread = spread(fps["native"]) if fps["native"] else None
    p99_spread = spread(p99["native"]) if p99["native"] else None
    row = dict(configuration=configuration, native_runs=str(len(valid["native"])), apk_runs=str(len(valid["apk"])),
               native_fps=number(native_fps, 2), apk_fps=number(apk_fps, 2), fps_ratio=number(fps_ratio, 4),
               native_p99_ms=ms(native_p99), apk_p99_ms=ms(apk_p99), p99_ratio=number(p99_ratio, 4),
               native_fps_spread=number(None if fps_spread is None else fps_spread * 100, 2),
               native_p99_spread=number(None if p99_spread is None else p99_spread * 100, 2),
               native_cpu_p50_ms=ms(nearest_rank(cpu["native"], 50)), apk_cpu_p50_ms=ms(nearest_rank(cpu["apk"], 50)))
    layers = {variant: sorted({run.layer for run in valid[variant]}) for variant in valid}
    if layers["native"] and layers["apk"] and layers["native"] != layers["apk"]:
        return dict(row, verdict="pair differs", reason=f"layers native {' '.join(layers['native'])}, "
                                                        f"apk {' '.join(layers['apk'])}")
    if len(valid["native"]) < VALID_RUNS or len(valid["apk"]) < VALID_RUNS:
        return dict(row, verdict="incomplete", reason=f"valid runs: native {len(valid['native'])}, "
                                                      f"apk {len(valid['apk'])}, of {VALID_RUNS} each")
    if fps_spread > NOISE or p99_spread > NOISE:
        spreads = (f"the native runs spread {float(fps_spread) * 100:.2f} % in FPS, "
                   f"{float(p99_spread) * 100:.2f} % in p99")
        if len(valid["native"]) < 2 * VALID_RUNS:
            return dict(row, verdict="more runs",
                        reason=f"{spreads}: 5 more runs per variant (tools/benchmark.sh --more {configuration})")
        return dict(row, verdict="undecided", reason=f"{spreads} over {len(valid['native'])} runs")
    misses = []
    if fps_ratio < FPS_LEAST:
        misses.append(f"FPS {float(fps_ratio) * 100:.2f} % of the native's")
    if p99_ratio > P99_MOST:
        misses.append(f"p99 {float(p99_ratio) * 100:.2f} % of the native's")
    return dict(row, verdict="fail" if misses else "pass", reason="; ".join(misses))


def write(path, columns, rows):
    with open(path, "w", newline="") as file:
        writer = csv.DictWriter(file, columns, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("results", nargs="?", default="build/benchmark")
    parser.add_argument("--out")
    args = parser.parse_args()
    results = pathlib.Path(args.results)
    out = pathlib.Path(args.out) if args.out else results / "summary"
    folders = sorted(folder for folder in results.glob("*") if (folder / "run.csv").is_file())
    if not folders:
        print(f"no run folders with a run.csv in {results}", file=sys.stderr)
        return 2
    runs = [run for run in (Run(folder) for folder in folders) if run.number != 0]
    configurations = sorted({run.configuration for run in runs})
    verdicts = [verdict(configuration, [run for run in runs if run.configuration == configuration])
                for configuration in configurations]
    out.mkdir(parents=True, exist_ok=True)
    write(out / "cold.csv", WINDOW_COLUMNS, [run.window_row("cold") for run in runs])
    write(out / "warm.csv", WINDOW_COLUMNS, [run.window_row("warm") for run in runs])
    write(out / "verdict.csv", VERDICT_COLUMNS, verdicts)

    for run in runs:
        if not run.valid:
            print(f"invalid  {run.name}: {', '.join(run.reasons)}")
    for row in verdicts:
        print(f"{row['configuration']:17} {row['verdict']:12} FPS {row['apk_fps'] or '-'} of "
              f"{row['native_fps'] or '-'}, p99 {row['apk_p99_ms'] or '-'} ms of {row['native_p99_ms'] or '-'} ms  "
              f"{row['reason']}")
    judged = {row["configuration"]: row["verdict"] for row in verdicts}
    if all(judged.get(configuration) == "pass" for configuration in SERIES):
        print("parity: holds")
    elif "fail" in judged.values():
        print("parity: fails")
    else:
        print("parity: not decided")
    print(f"summary in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
