#!/usr/bin/env python3
"""Render the proposal's benchmark figures from retained measurements."""

import argparse
from collections import defaultdict
from html import escape
import json
import os
from pathlib import Path
import re
import statistics

BOOK = Path(__file__).resolve().parents[1]
EVIDENCE = BOOK.parent / "prototype/bench/vsock"
os.environ.setdefault("MPLCONFIGDIR", str(BOOK / ".cache/matplotlib"))

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


SIZES = (4, 16, 64)
TRANSPORTS = ("pipe", "klet", "virtio")
LABELS = {"pipe": "Pipe", "klet": "Klet (mode 2)", "virtio": "Virtio-vsock"}
COLORS = {"pipe": "#0072B2", "klet": "#009E73", "virtio": "#D55E00"}
MARKERS = {"pipe": "o", "klet": "s", "virtio": "^"}
STYLES = {"pipe": "--", "klet": "-", "virtio": ":"}
MODE_COLORS = {0: "#7A5195", 1: "#C48200", 2: COLORS["klet"]}


def measurements():
    """Group all six boots without rounding the measured values."""
    groups = defaultdict(list)
    for label in ("0a", "1a", "2a", "2b", "1b", "0b"):
        run = EVIDENCE / f"final-{label}"
        metadata = json.loads((run / "environment.json").read_text())
        assert not metadata["profiling"] and metadata["mode"] == "compare"
        mode = metadata["notification_mode"]
        assert mode == int(label[0])
        rows = []
        for line in (run / "serial.txt").read_text().splitlines():
            if "BENCH transport=" not in line:
                continue
            raw = dict(re.findall(r"(\w+)=(\w+)", line))
            row = {k: v if k == "transport" else int(v) for k, v in raw.items()}
            assert row["rounds"] == 8192
            assert row["bytes"] == 2 * row["size"] * row["rounds"]
            groups[mode, row["size"] // 1024, row["transport"]].append(row)
            rows.append(row)
        assert len(rows) == 84, f"incomplete run: {run}"
        for size in SIZES:
            for transport in (*TRANSPORTS, "api"):
                assert sorted(r["repeat"] for r in rows
                              if r["size"] == size * 1024 and r["transport"] == transport) == list(range(1, 8))
    assert len(groups) == 36 and all(len(rows) == 14 for rows in groups.values())
    return groups


def values(groups, mode, transport, metric):
    def get(row):
        if metric == "goodput":
            return row["bytes"] * 1e9 / row["wall_ns"] / 2**20
        if metric in ("p50", "p95"):
            return row[f"{metric}_ns"] / 1000
        if metric == "endpoint":
            return (row["cpu_ns"] + row["peer_cpu_ns"]) / row["bytes"]
        if metric == "outer":
            return sum(row[key] for key in ("outer_qemu_ns", "outer_vhost_ns", "outer_peer_ns")) / row["bytes"]
        raise ValueError(metric)

    return [[get(row) for row in groups[mode, size, transport]] for size in SIZES]


def curve(ax, samples, label, color, marker, linestyle, show_range=False):
    medians = [statistics.median(group) for group in samples]
    errors = None
    if show_range:
        errors = ([median - min(group) for median, group in zip(medians, samples)],
                  [max(group) - median for median, group in zip(medians, samples)])
    ax.errorbar(SIZES, medians, yerr=errors, label=label, color=color,
                marker=marker, linestyle=linestyle, linewidth=2, markersize=6,
                capsize=3, elinewidth=1)
    return medians


def size_axis(ax, title, ylabel, limit):
    ax.set_title(title, loc="left", fontsize=12, pad=12)
    ax.set_xscale("log", base=2)
    ax.set_xticks(SIZES, [str(size) for size in SIZES])
    ax.set_xlim(3.3, 78)
    ax.set_ylim(*limit)
    ax.set_xlabel("Requested transfer size (KiB)")
    ax.set_ylabel(ylabel)
    ax.grid(axis="y", color="#dce1e6", linewidth=0.8)
    ax.set_axisbelow(True)


def save(fig, output, name, title, description):
    path = output / f"{name}.svg"
    fig.savefig(path, format="svg", facecolor="white",
                metadata={"Date": None, "Creator": "book/tools/plot-vsock.py"})
    # Keep text accessible when the SVG is opened independently of the book.
    svg = path.read_text()
    svg = re.sub(r"(<svg\b[^>]*>)", lambda match: match[1] +
                 f"\n<title>{escape(title)}</title>\n<desc>{escape(description)}</desc>", svg, count=1)
    path.write_text(svg)
    plt.close(fig)
    print(path.relative_to(BOOK.parent) if path.is_relative_to(BOOK.parent) else path)


def compare(groups, output):
    fig, axes = plt.subplots(1, 2, figsize=(9.2, 4.2), layout="constrained")
    size_axis(axes[0], "Echo goodput · higher is better", "Goodput (MiB/s)", (0, 3700))
    size_axis(axes[1], "Klet relative to each reference", "Ratio of median goodput", (0.7, 2.0))
    medians = {}
    for transport in TRANSPORTS:
        medians[transport] = curve(axes[0], values(groups, 2, transport, "goodput"),
                                  LABELS[transport], COLORS[transport], MARKERS[transport],
                                  STYLES[transport], show_range=True)
    axes[0].legend(loc="upper left")
    axes[1].axhline(1, color="#687078", linewidth=1, linestyle="--")
    for reference in ("pipe", "virtio"):
        ratios = [klet / base for klet, base in zip(medians["klet"], medians[reference])]
        axes[1].plot(SIZES, ratios, color=COLORS[reference], marker=MARKERS[reference],
                     linestyle=STYLES[reference], linewidth=2, label=f"Klet / {LABELS[reference]}")
        for size, ratio in zip(SIZES, ratios):
            axes[1].annotate(f"{ratio:.2f}×", (size, ratio), xytext=(0, 9),
                             textcoords="offset points", ha="center", color=COLORS[reference])
    axes[1].legend(loc="upper left")
    save(fig, output, "goodput", "Echo goodput and relative performance",
         "Mode 2 comparison at 4, 16, and 64 KiB. Points are medians of 14 measurements; "
         "whiskers show minima and maxima, not confidence intervals. Klet/virtio ratios are "
         "0.85, 1.32, and 1.80. Lines only connect measured sizes.")

    for name, metrics, titles, ylabel, limit in (
        ("latency", ("p50", "p95"), ("p50 RTT", "p95 RTT"),
         "Round-trip latency (µs)", (0, 105)),
        ("cpu", ("endpoint", "outer"), ("Endpoint CPU", "Outer-task CPU"),
         "CPU time per payload byte (ns/byte)", (0, 6.6)),
    ):
        fig, axes = plt.subplots(1, 2, figsize=(9.2, 4.1), layout="constrained")
        for ax, metric, title in zip(axes, metrics, titles):
            size_axis(ax, title + " · lower is better", ylabel, limit)
            for transport in TRANSPORTS:
                curve(ax, values(groups, 2, transport, metric), LABELS[transport],
                      COLORS[transport], MARKERS[transport], STYLES[transport])
            ax.legend(loc="upper left" if name == "latency" else "upper right")
        description = ("Mode 2, 14 measurements per size and transport. Each latency point is a median "
                       "of per-case percentiles, not a pooled percentile." if name == "latency" else
                       "Mode 2 median CPU per both-direction payload byte. Endpoint and outer-task "
                       "scopes overlap and must not be added. Outer-task CPU includes KVM halt polling "
                       "and a small control interval. Neither scope is all-machine or transport-only CPU.")
        save(fig, output, name, "Round-trip latency" if name == "latency" else "CPU accounting scopes", description)


def ablation(groups, output):
    fig, axes = plt.subplots(1, 2, figsize=(9.2, 4.2), layout="constrained")
    size_axis(axes[0], "Notification-policy ablation", "Klet echo goodput (MiB/s)", (0, 3700))
    size_axis(axes[1], "Optimized versus reference", "Ratio of median goodput", (0.8, 2.55))
    medians = {}
    for mode, marker, style in ((0, "o", ":"), (1, "^", "--"), (2, "s", "-")):
        medians[mode] = curve(axes[0], values(groups, mode, "klet", "goodput"), f"Mode {mode}",
                             MODE_COLORS[mode], marker, style, show_range=True)
    axes[0].legend(loc="upper left")
    ratios = [new / old for new, old in zip(medians[2], medians[0])]
    axes[1].axhline(1, color="#687078", linewidth=1, linestyle="--")
    axes[1].plot(SIZES, ratios, color=MODE_COLORS[2], marker="s", linewidth=2, label="Mode 2 / mode 0")
    for size, ratio in zip(SIZES, ratios):
        axes[1].annotate(f"{ratio:.2f}×", (size, ratio), xytext=(0, 10),
                         textcoords="offset points", ha="center", color=MODE_COLORS[2])
    axes[1].legend(loc="upper left")
    save(fig, output, "ablation", "Notification optimization benefit",
         "Median goodput across 14 measurements per size and mode; whiskers are observed ranges. "
         "Mode 1 suppresses reservation wakes. Mode 2 also separates poll and gates blocking commit wakes. "
         "Mode 2/reference gains are 1.25, 1.79, and 2.20 times. Lines only connect measured sizes.")


def profile(output):
    rows = json.loads((EVIDENCE / "profile-summary.json").read_text())
    selected = {row["mode"]: row for row in rows if row["transport"] == "klet" and row["size"] == 65536}
    assert set(selected) == {0, 2}
    counts = ("blocking_data_notifications", "poll_notifies", "waits", "slept", "retry_eagain")
    labels = ("Blocking data notifications", "Separate poll notifications", "Blocking wait entries",
              "Voluntary switches in waits", "Unsuccessful retries")
    times = {}
    for mode in (0, 2):
        text = (EVIDENCE / f"profile-mode-{mode}/serial.txt").read_text()
        block = text.split("PROFILE-BEGIN transport=klet size=65536 ", 1)[1].split("PROFILE-BEGIN", 1)[0]
        copies = [{k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
                  for line in block.splitlines() if "VSOCK-COPY " in line]
        assert len(copies) == 2 and {row["tx"] for row in copies} == {0, 1}
        times[mode] = [selected[mode]["host_copy_ns"] / 1e6,
                       next(row["copy_ns"] for row in copies if row["tx"] == 0) / 1e6,
                       next(row["copy_ns"] for row in copies if row["tx"] == 1) / 1e6,
                       selected[mode]["lookup_ns"] / 1e6]
    fig, axes = plt.subplots(2, 1, figsize=(9.2, 6.8), layout="constrained")
    panels = ((axes[0], labels, {mode: [selected[mode][key] for key in counts] for mode in (0, 2)},
               "Notification and waiting counts", "Count", 40000, lambda value: f"{value:,.0f}"),
              (axes[1], ("Host payload copy", "vOSTD read payload copy", "vOSTD write payload copy", "Handle lookup"),
               times, "Copy and lookup elapsed intervals", "Aggregate elapsed time (ms)", 26,
               lambda value: f"{value:.2f}"))
    for ax, categories, data, title, xlabel, xmax, fmt in panels:
        for mode, offset in ((0, -0.19), (2, 0.19)):
            positions = [i + offset for i in range(len(categories))]
            ax.barh(positions, data[mode], height=0.32, color=MODE_COLORS[mode],
                    hatch="///" if mode == 0 else None, label=f"Mode {mode}", zorder=3)
            for y, value in zip(positions, data[mode]):
                ax.text(value + xmax * 0.012, y, fmt(value), va="center", fontsize=10)
        ax.set_yticks(range(len(categories)), categories)
        ax.invert_yaxis()
        ax.set_xlim(0, xmax)
        ax.set_title(title, loc="left", fontsize=12, pad=12)
        ax.set_xlabel(xlabel)
        ax.grid(axis="x", color="#dce1e6", linewidth=0.8)
        ax.set_axisbelow(True)
        ax.legend(loc="lower right")
    axes[0].xaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:,.0f}"))
    save(fig, output, "profile", "64 KiB tenant-echo diagnostics",
         "Separate instrumented runs: 4096 timed plus 64 warmup RTTs. Counts include setup and teardown. "
         "The near-zero retry count is one, not zero. Elapsed intervals can include preemption, overlap, "
         "and must not be added as a CPU or round-trip-time partition. Lookup covers 16646 calls.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        default=BOOK / "src/design-proposals/figures/klet-vsock")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "legend.frameon": False, "legend.fontsize": 10,
                         "svg.fonttype": "none", "svg.hashsalt": "klet-vsock-benchmarks"})
    groups = measurements()
    compare(groups, args.output_dir)
    ablation(groups, args.output_dir)
    profile(args.output_dir)


if __name__ == "__main__":
    main()
