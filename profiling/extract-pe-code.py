"""Extract logged static shader code by RVA from the game's on-disk PE image."""
import argparse
import struct
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("image", type=Path)
p.add_argument("rva", type=lambda s: int(s, 0))
p.add_argument("words", type=int)
p.add_argument("output", type=Path)
a = p.parse_args()
data = a.image.read_bytes()
pe = struct.unpack_from("<I", data, 0x3c)[0]
assert data[pe:pe+4] == b"PE\0\0"
count = struct.unpack_from("<H", data, pe+6)[0]
optional_size = struct.unpack_from("<H", data, pe+20)[0]
sections = pe+24+optional_size
size = a.words*4
assert size > 0
for index in range(count):
    section = sections+40*index
    virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", data, section+8)
    if rva <= a.rva and a.rva+size <= rva+raw_size:
        offset = raw_offset+a.rva-rva
        payload = data[offset:offset+size]
        assert len(payload) == size
        a.output.write_bytes(payload)
        print(f"{a.output}: {size} bytes, RVA {a.rva:#x}, file offset {offset:#x}")
        break
else:
    raise ValueError("Code range is not fully backed by a PE section")
