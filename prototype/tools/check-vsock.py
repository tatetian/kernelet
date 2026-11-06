#!/usr/bin/env python3
"""Check the integrated experiment and summarize paired performance samples."""
import re
import statistics
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text()
required = [
    "PASS lifecycle, rings, backpressure, peer death, survivors, end-to-end measurements",
    "ok independent kernelet peers verified",
    "ok host listener accepts",
    "ok default denial, explicit permission, and dead CID refusal",
    "ok partial user faults",
    "ok surviving kernelet observes reset",
    "ok reservation survives peer destruction",
    "ok reservation survives concurrent shutdown until publication",
    "ok controlled consumption wakes full-ring writer without peer death",
    "ok same-endpoint TX lease commit wakes registered contender",
    "ok same-endpoint TX lease zero cancellation wakes registered contender",
    "ok same-endpoint RX lease commit wakes registered contender",
    "ok same-endpoint RX lease zero cancellation wakes registered contender",
    "ok SHUT_WR plus zero TX cancellation enables peer EOF",
    "ok epoll-only zero TX lease cancellation restores readiness",
    "ok epoll-only zero RX lease cancellation restores readiness",
    "ok interrupted reader balances demand",
    "ok interrupted writer balances demand",
    "ok poll-only RX publication",
    "ok level epoll-only RX publication",
    "ok edge epoll-only RX publication",
    "ok epoll-only full-to-writable consumption",
    "ok killed waiting image unregisters demand",
    "ok accept wakes connector after bounded backlog release",
    "ok concurrent independent rings stress check-register-commit retries",
    "ok kill wakes blocked reader",
    "ok kill wakes blocked writer",
    "ok kill wakes blocked connect with full backlog",
    "VSOCK: 0 endpoints live, 0 connections live",
    "INIT: done",
    "kernelet: endovisor unloaded",
]
for marker in required:
    if marker not in text:
        sys.exit(f"FAIL: missing {marker!r}")
if re.search(r"WARNING:|BUG:|Oops:|panicked|FAIL |delete_module endovisor failed", text):
    sys.exit("FAIL: kernel or experiment reported a failure")

rows = []
for line in text.splitlines():
    if "BENCH transport=" in line:
        row = dict(re.findall(r"(\w+)=(\w+)", line))
        rows.append({key: value if key == "transport" else int(value)
                     for key, value in row.items()})
if len(rows) != 27:
    sys.exit(f"FAIL: expected 27 measurements, got {len(rows)}")
print("size | transport | median MiB/s | median p50 us | median p95 us | median CPU ns/byte")
for size in (4096, 16384, 65536):
    for transport in ("pipe", "klet", "api"):
        group = [r for r in rows if r["size"] == size and r["transport"] == transport]
        assert sorted(r["repeat"] for r in group) == [1, 2, 3]
        assert all(r["bytes"] == 2 * size * r["rounds"] and r["rounds"] == 1024 for r in group)
        assert all(r["wall_ns"] > 0 and r["cpu_ns"] > 0 and r["peer_cpu_ns"] > 0 for r in group)
        throughput = statistics.median(r["bytes"] * 1e9 / r["wall_ns"] / 2**20 for r in group)
        p50 = statistics.median(r["p50_ns"] / 1000 for r in group)
        p95 = statistics.median(r["p95_ns"] / 1000 for r in group)
        cpu = statistics.median((r["cpu_ns"] + r["peer_cpu_ns"]) / r["bytes"] for r in group)
        print(f"{size} | {transport} | {throughput:.1f} | {p50:.2f} | {p95:.2f} | {cpu:.3f}")
print("PASS: lifecycle, peer failure, reservation races, cleanup, and 27 measurements")
