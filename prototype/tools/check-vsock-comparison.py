#!/usr/bin/env python3
"""Validate retained comparisons and summarize endpoint and outer-task CPU."""
import argparse
import ast
import json
from pathlib import Path
import re
import statistics

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("runs", nargs="+", type=Path)
args = parser.parse_args()
required = next(ast.literal_eval(node.value)
                for node in ast.parse((ROOT / "tools/check-vsock.py").read_text()).body
                if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "required"
                                                        for t in node.targets))
all_rows = []
for run in args.runs:
    metadata = json.loads((run / "environment.json").read_text())
    text = (run / "serial.txt").read_text()
    assert metadata["mode"] == "compare" and not metadata["profiling"]
    assert all(marker in text for marker in required), f"missing correctness marker in {run}"
    assert not re.search(r"WARNING:|BUG:|Oops:|panicked|FAIL |delete_module endovisor failed", text)
    assert f"notification_mode={metadata['notification_mode']}" in text
    assert f"local_cid={metadata['guest_cid']} peer_cid=2 buffer_bytes=65536" in text
    irq_lines = [line for line in text.splitlines() if "VIRTIO-IRQ irq=" in line]
    assert len(irq_lines) >= 2 and all("PCI-MSI" in line and "requested=1" in line
                                       for line in irq_lines), "MSI and requested IRQ affinity required"
    data_irqs = [line for line in irq_lines if "-config" not in line]
    assert data_irqs and all("effective=1" in line for line in data_irqs)
    after_irqs = [line for line in text.splitlines() if "VIRTIO-IRQ-AFTER" in line]
    for before in data_irqs:
        irq, cpu0, cpu1 = map(int, re.search(r"line=\s*(\d+):\s+(\d+)\s+(\d+)", before).groups())
        after = next(line for line in after_irqs if f"irq={irq} " in line)
        _, final0, final1 = map(int, re.search(r"line=\s*(\d+):\s+(\d+)\s+(\d+)", after).groups())
        assert "effective=1" in after and final0 == cpu0 and final1 > cpu1, "observed data IRQ delivery required"
    assert metadata["caller_cpu"] != metadata["peer_cpu"]
    for cpu in metadata["vcpu_threads"]:
        expected = metadata["peer_cpu"] if cpu["cpu-index"] == 0 else metadata["caller_cpu"]
        assert metadata["qemu_affinity"][str(cpu["thread-id"])] == [expected]
    assert metadata["vhost_workers"]
    assert all(worker["affinity"] == [metadata["peer_cpu"]] for worker in metadata["vhost_workers"])
    rows = []
    for line in text.splitlines():
        if "BENCH transport=" in line:
            raw = dict(re.findall(r"(\w+)=(\w+)", line))
            row = {k: v if k == "transport" else int(v) for k, v in raw.items()}
            row["mode"] = metadata["notification_mode"]
            rows.append(row)
    assert len(rows) == 84, f"expected 84 measurements in {run}, got {len(rows)}"
    outer = []
    for line in (run / "outer.txt").read_text().splitlines():
        if line.startswith("OUTER "):
            outer.append({k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)})
    assert len(outer) == len(rows)
    for row, external in zip(rows, outer):
        assert external["transport"] == {"klet": 0, "pipe": 1, "api": 2, "virtio": 3}[row["transport"]]
        assert external["size"] == row["size"] and external["repeat"] == row["repeat"]
        for source, target in (("qemu_ns", "outer_qemu_ns"), ("vhost_ns", "outer_vhost_ns"),
                               ("peer_ns", "outer_peer_ns"), ("wall_ns", "outer_wall_ns")):
            assert external[source] == row[target]
        assert row["rounds"] == 8192 and row["bytes"] == 2 * row["size"] * row["rounds"]
        assert row["outer_wall_ns"] >= row["wall_ns"] > 0
        assert row["cpu_ns"] > 0 and row["peer_cpu_ns"] > 0 and row["outer_vhost_ns"] > 0
        assert external["workers"] == len(metadata["vhost_workers"])
    for size in (4096, 16384, 65536):
        for transport in ("pipe", "klet", "api", "virtio"):
            assert sorted(r["repeat"] for r in rows if r["size"] == size and r["transport"] == transport) == list(range(1, 8))
    print(f"PASS {run}: complete correctness suite, 84 matched measurements, MSI and affinity, CPU windows")
    all_rows.extend(rows)

print("mode size transport MiB/s[min,max] RTT_p50_us RTT_p95_us endpoint_ns/B outer_tasks_ns/B")
for mode in sorted({r["mode"] for r in all_rows}):
    for size in (4096, 16384, 65536):
        for transport in ("pipe", "klet", "api", "virtio"):
            group = [r for r in all_rows if (r["mode"], r["size"], r["transport"]) == (mode, size, transport)]
            if not group:
                continue
            speeds = [r["bytes"] * 1e9 / r["wall_ns"] / 2**20 for r in group]
            p50 = statistics.median(r["p50_ns"] / 1000 for r in group)
            p95 = statistics.median(r["p95_ns"] / 1000 for r in group)
            endpoints = statistics.median((r["cpu_ns"] + r["peer_cpu_ns"]) / r["bytes"] for r in group)
            outer = statistics.median((r["outer_qemu_ns"] + r["outer_vhost_ns"] + r["outer_peer_ns"])
                                      / r["bytes"] for r in group)
            print(f"{mode} {size} {transport} {statistics.median(speeds):.1f}[{min(speeds):.1f},{max(speeds):.1f}]"
                  f" {p50:.2f} {p95:.2f} {endpoints:.3f} {outer:.3f}")
