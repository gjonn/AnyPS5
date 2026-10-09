"""Read a just-reported slow shader twice from a live, authorized profiling process."""
import ctypes
import hashlib
import json
import re
import sys
import time
from pathlib import Path

pid, log, output = int(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
k = ctypes.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_bool, ctypes.c_ulong]
k.OpenProcess.restype = ctypes.c_void_p
k.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k.CloseHandle.argtypes = [ctypes.c_void_p]
handle = k.OpenProcess(0x10, False, pid)
if not handle:
    raise ctypes.WinError(ctypes.get_last_error())
results = []
pattern = re.compile(rb"\[control-flow\] validation=(\d+) code=0x([0-9a-f]+).*?words=(\d+) ms=([\d.]+)")
try:
    with log.open("rb") as stream:
        stream.seek(0, 2)
        pending = b""
        deadline = time.monotonic()+45
        while len(results) < 4 and time.monotonic() < deadline:
            pending += stream.read(1 << 20)
            lines = pending.split(b"\n")
            pending = lines.pop()
            for line in lines:
                m = pattern.search(line)
                if not m or float(m[4]) < 100 or int(m[3]) != 5756:
                    continue
                address, size = int(m[2], 16), int(m[3])*4
                payloads = []
                for _ in range(2):
                    buffer = ctypes.create_string_buffer(size)
                    count = ctypes.c_size_t()
                    if not k.ReadProcessMemory(handle, address, buffer, size, ctypes.byref(count)) or count.value != size:
                        break
                    payloads.append(buffer.raw)
                if len(payloads) != 2 or payloads[0] != payloads[1]:
                    continue
                path = output / f"live-validation-{int(m[1])}.bin"
                path.write_bytes(payloads[0])
                row = dict(validation=int(m[1]), address=hex(address), bytes=size, sha256=hashlib.sha256(payloads[0]).hexdigest(), path=str(path))
                results.append(row)
                print(json.dumps(row), flush=True)
                if len(results) >= 4:
                    break
            time.sleep(.005)
finally:
    k.CloseHandle(handle)
(output / "live-captures.json").write_text(json.dumps(results, indent=2))
