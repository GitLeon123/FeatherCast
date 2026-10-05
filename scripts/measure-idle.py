"""Read-only Windows process counters; keep the target hidden during sampling."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import hashlib
import os
import platform
from pathlib import Path
from datetime import datetime, timezone
import time

parser = argparse.ArgumentParser()
parser.add_argument("pid", type=int)
parser.add_argument("--seconds", type=float, default=60)
parser.add_argument("--label", default="hidden-idle", help="Operator-described scenario, e.g. phone-connected")
parser.add_argument("--binary", type=Path, help="Built executable, recorded by SHA-256")
parser.add_argument("--output", type=Path, help="Write the same JSON report to a file")
args = parser.parse_args()
if not 1 <= args.seconds <= 3600:
    parser.error("--seconds must be between 1 and 3600")
kernel = c.WinDLL("kernel32", use_last_error=True)
psapi = c.WinDLL("psapi", use_last_error=True)
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.CloseHandle.argtypes = [w.HANDLE]

class Memory(c.Structure):
    _fields_ = [("cb", w.DWORD), ("faults", w.DWORD)] + [
        (name, c.c_size_t) for name in ("peakWorking", "working", "peakPaged",
        "paged", "peakNonpaged", "nonpaged", "pagefile", "peakPagefile", "private")]

class Io(c.Structure):
    _fields_ = [(name, c.c_ulonglong) for name in (
        "readOps", "writeOps", "otherOps", "readBytes", "writeBytes", "otherBytes")]

kernel.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(c.c_ulonglong)] * 4
kernel.GetProcessIoCounters.argtypes = [w.HANDLE, c.POINTER(Io)]
psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, c.POINTER(Memory), w.DWORD]
handle = kernel.OpenProcess(0x410, False, args.pid)
if not handle:
    raise c.WinError(c.get_last_error())

def sample():
    times = [c.c_ulonglong() for _ in range(4)]
    memory, io = Memory(), Io()
    memory.cb = c.sizeof(memory)
    if not (kernel.GetProcessTimes(handle, *(c.byref(t) for t in times)) and
            kernel.GetProcessIoCounters(handle, c.byref(io)) and
            psapi.GetProcessMemoryInfo(handle, c.byref(memory), memory.cb)):
        raise c.WinError(c.get_last_error())
    return dict(cpuMs=(times[2].value + times[3].value) / 10000,
                privateBytes=memory.private, workingBytes=memory.working,
                pageFaults=memory.faults, **{name: getattr(io, name) for name, _ in Io._fields_})

try:
    before = sample()
    started = time.monotonic()
    samples = [before]
    while time.monotonic() - started < args.seconds:
        time.sleep(min(1, max(0, args.seconds - (time.monotonic() - started))))
        samples.append(sample())
    after = samples[-1]
    elapsed = time.monotonic() - started
    cpu_ms = after["cpuMs"] - before["cpuMs"]
    report = dict(schemaVersion=1, utc=datetime.now(timezone.utc).isoformat(),
        scenario=args.label, pid=args.pid, seconds=elapsed, sampleCount=len(samples),
        environment=dict(os=platform.platform(), architecture=platform.machine(),
                         logicalProcessors=os.cpu_count(), cpu=os.environ.get("PROCESSOR_IDENTIFIER", "unknown")),
        cpuMs=cpu_ms, cpuPercentOneCore=100 * cpu_ms / (elapsed * 1000),
        privateBytes=after["privateBytes"], workingBytes=after["workingBytes"],
        peakSampledPrivateBytes=max(s["privateBytes"] for s in samples),
        peakSampledWorkingBytes=max(s["workingBytes"] for s in samples),
        delta={key: after[key] - before[key] for key in before})
    if args.binary:
        digest = hashlib.sha256()
        with args.binary.open("rb") as binary:
            for chunk in iter(lambda: binary.read(1024 * 1024), b""):
                digest.update(chunk)
        report["binary"] = dict(name=args.binary.name, sha256=digest.hexdigest())
    encoded = json.dumps(report, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
finally:
    kernel.CloseHandle(handle)
