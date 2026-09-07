"""Read-only Windows process counters; keep the target hidden during sampling."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import time

parser = argparse.ArgumentParser()
parser.add_argument("pid", type=int)
parser.add_argument("--seconds", type=float, default=60)
args = parser.parse_args()
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
    time.sleep(args.seconds)
    after = sample()
    print(json.dumps(dict(pid=args.pid, seconds=time.monotonic() - started,
        privateBytes=after["privateBytes"], workingBytes=after["workingBytes"],
        delta={key: after[key] - before[key] for key in before}), indent=2))
finally:
    kernel.CloseHandle(handle)
