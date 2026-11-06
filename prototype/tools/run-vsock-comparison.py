#!/usr/bin/env python3
"""Run a pinned, non-nested comparison and retain the complete environment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--label", required=True)
parser.add_argument("--mode", choices=("compare", "profile"), default="compare")
parser.add_argument("--optimization", type=int, choices=(0, 1, 2), default=0)
args = parser.parse_args()
if not args.label.replace("-", "").replace("_", "").isalnum():
    parser.error("label must be a simple directory name")
out = ROOT / ".build" / ("vsock-" + args.label)
out.mkdir(exist_ok=True)
if (out / "serial.txt").exists():
    parser.error(f"retained run already exists: {out}; choose another label")

cores = {}
for cpu in sorted(os.sched_getaffinity(0)):
    topo = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
    key = ((topo / "physical_package_id").read_text(), (topo / "core_id").read_text())
    cores.setdefault(key, cpu)
if len(cores) < 3:
    raise SystemExit("three distinct available physical cores are required")
peer_cpu, caller_cpu, house_cpu = list(cores.values())[:3]

with (out / "build.txt").open("w") as log:
    subprocess.run(["make", "KLET_KERNEL=vsocktest", "stage-vsock", "initramfs",
                    f"VSOCK_RUN_MODE={args.mode}",
                    f"VSOCK_PROFILE={int(args.mode == 'profile')}", f"VSOCK_OPT={args.optimization}", "JOBS=4"],
                   cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run(["gcc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-o", str(ROOT / ".build/vsock-peer"), "host/runtime/vsock-peer.c"],
                   cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)

qmp_path = out / "qmp.sock"
qmp_path.unlink(missing_ok=True)
cid = 100000 + os.getpid()
command = [os.environ.get("QEMU", "qemu-system-x86_64"),
           "-enable-kvm", "-cpu", "host", "-smp", "2", "-m", "512M",
           "-nographic", "-no-reboot", "-monitor", "none", "-S",
           "-qmp", f"unix:{qmp_path},server=on,wait=off",
           "-kernel", str(ROOT / ".build/linux-6.12/arch/x86/boot/bzImage"),
           "-initrd", str(ROOT / ".build/initramfs.cpio.gz"),
           "-append", "console=ttyS0 panic=-1",
           "-serial", f"file:{out / 'serial.txt'}",
           "-device", f"vhost-vsock-pci,guest-cid={cid},disable-legacy=on"]
metadata = {
    "label": args.label, "mode": args.mode, "qemu_command": command,
    "host_kernel": os.uname().release, "guest_cid": cid,
    "peer_cpu": peer_cpu, "caller_cpu": caller_cpu, "house_cpu": house_cpu,
    "topology_limit": "virtio peer is outer-host Linux; pipe/klet peers are guest Linux",
    "outer_window": "includes untimed control start acknowledgement and end token",
    "profiling": args.mode == "profile", "notification_mode": args.optimization,
    "source_sha256": {},
}
for path in sorted((ROOT / "host/runtime").glob("*vsock*")):
    metadata["source_sha256"][str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
for relative in ("linux/endovisor/vsock.c", "linux/config.fragment", "vostd/src/vsock.rs",
                 "vostd/src/mm/io.rs", "vostd/src/abi.rs", "linux/endovisor/kernelet.h",
                 "linux/endovisor/carrier.c", "linux/endovisor/sandbox.c", "linux/endovisor/main.c",
                 "linux/endovisor/uapi/klet-vsock.h", "Makefile", "kernels/vsock/src/lib.rs",
                 "kernels/vsock/tenant.S", "tools/run-vsock-comparison.py"):
    metadata["source_sha256"][relative] = hashlib.sha256((ROOT / relative).read_bytes()).hexdigest()
metadata["binary_sha256"] = {
    relative: hashlib.sha256((ROOT / relative).read_bytes()).hexdigest()
    for relative in (".build/linux-6.12/arch/x86/boot/bzImage", ".build/linux-6.12/.config",
                     "linux/endovisor/endovisor.ko", ".build/vsocktest.image", ".build/vsock-peer")
}
metadata["cpu_model"] = next(line for line in Path("/proc/cpuinfo").read_text().splitlines()
                             if line.startswith("model name"))
metadata["load_before"] = Path("/proc/loadavg").read_text().strip()
metadata["host_halt_poll_ns"] = Path("/sys/module/kvm/parameters/halt_poll_ns").read_text().strip()
metadata["guest_virtio_irq_cpu"] = 1
metadata["cpu_governor"] = {
    cpu: (Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor").read_text().strip()
          if Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor").exists() else "unavailable")
    for cpu in (peer_cpu, caller_cpu, house_cpu)
}
metadata["qemu_version"] = subprocess.check_output([command[0], "--version"], text=True).splitlines()[0]

qemu = host = connection = None
try:
    qemu_log = (out / "qemu.txt").open("w")
    host_log = (out / "outer.txt").open("w")
    qemu = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL, stdout=qemu_log, stderr=qemu_log)
    deadline = time.monotonic() + 15
    while not qmp_path.exists() and time.monotonic() < deadline and qemu.poll() is None:
        time.sleep(0.05)
    connection = socket.socket(socket.AF_UNIX)
    connection.settimeout(10)
    connection.connect(str(qmp_path))
    stream = connection.makefile("rwb", buffering=0)
    stream.readline()

    def qmp(name):
        stream.write(json.dumps({"execute": name}).encode() + b"\n")
        while True:
            response = json.loads(stream.readline())
            if "error" in response:
                raise RuntimeError(response)
            if "return" in response:
                return response["return"]

    qmp("qmp_capabilities")
    cpus = qmp("query-cpus-fast")
    tids = {int(p.name) for p in Path(f"/proc/{qemu.pid}/task").iterdir()}
    for tid in tids:
        os.sched_setaffinity(tid, {house_cpu})
    for cpu in cpus:
        os.sched_setaffinity(cpu["thread-id"], {peer_cpu if cpu["cpu-index"] == 0 else caller_cpu})

    def find_vhosts():
        found = {}
        for task in Path(f"/proc/{qemu.pid}/task").iterdir():
            comm = (task / "comm").read_text().strip()
            if comm.startswith("vhost-"):
                found[int(task.name)] = comm
        for proc in Path("/proc").iterdir():
            if not proc.name.isdigit():
                continue
            try:
                comm = (proc / "comm").read_text().strip()
                status = (proc / "status").read_text()
                parent = next(int(line.split()[1]) for line in status.splitlines() if line.startswith("PPid:"))
                if comm == f"vhost-{qemu.pid}" or (comm.startswith("vhost-") and parent == qemu.pid):
                    found[int(proc.name)] = comm
            except (FileNotFoundError, ProcessLookupError):
                continue
        return found

    found = find_vhosts()
    resumed = False
    if not found:
        qmp("cont")
        resumed = True
        deadline = time.monotonic() + 10
        while not found and time.monotonic() < deadline:
            time.sleep(0.02)
            found = find_vhosts()
    if not found:
        raise RuntimeError("could not identify the dedicated vhost worker")
    vhosts = []
    for tid, comm in found.items():
        os.sched_setaffinity(tid, {peer_cpu})
        vhosts.append({"pid": tid, "comm": comm, "affinity": sorted(os.sched_getaffinity(tid))})
    tids = {int(p.name) for p in Path(f"/proc/{qemu.pid}/task").iterdir()}
    metadata["qemu_pid"] = qemu.pid
    metadata["vcpu_threads"] = cpus
    metadata["qemu_affinity"] = {tid: sorted(os.sched_getaffinity(tid)) for tid in tids}
    metadata["vhost_workers"] = vhosts
    host = subprocess.Popen([str(ROOT / ".build/vsock-peer"), str(qemu.pid), str(peer_cpu), ",".join(str(v["pid"]) for v in vhosts)],
                            cwd=ROOT, stdout=host_log, stderr=host_log)
    deadline = time.monotonic() + 10
    while "VSOCK-PEER ready" not in (out / "outer.txt").read_text() and time.monotonic() < deadline:
        if host.poll() is not None:
            raise RuntimeError((out / "outer.txt").read_text())
        time.sleep(0.05)
    if not resumed:
        qmp("cont")
    print(f"running {args.label}: caller core {caller_cpu}, peer core {peer_cpu}, vhost {vhosts}", flush=True)
    deadline = time.monotonic() + 600
    while qemu.poll() is None and time.monotonic() < deadline:
        if host.poll() is not None:
            raise RuntimeError((out / "outer.txt").read_text())
        time.sleep(0.2)
    if qemu.poll() is None:
        raise RuntimeError("QEMU comparison timed out")
    if qemu.returncode:
        raise RuntimeError(f"QEMU exited {qemu.returncode}")
    metadata["load_after"] = Path("/proc/loadavg").read_text().strip()
    print(f"completed {args.label}: {out}", flush=True)
finally:
    if connection:
        connection.close()
    for process in (host, qemu):
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    (out / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
