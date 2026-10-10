import glob
import math
import os
import struct
import sys
from PIL import Image

src = sys.argv[1]
dst = sys.argv[2]
step = int(sys.argv[3]) if len(sys.argv) > 3 else 4
os.makedirs(dst, exist_ok=True)


def half(bits):
    return struct.unpack('<e', struct.pack('<H', bits))[0]


def small_float(bits, mantissa_bits):
    exponent = bits >> mantissa_bits
    mantissa = bits & ((1 << mantissa_bits) - 1)
    if exponent == 0:
        return mantissa / (1 << mantissa_bits) * 2.0 ** -14
    if exponent == 31:
        return float('inf') if mantissa == 0 else float('nan')
    return (1 + mantissa / (1 << mantissa_bits)) * 2.0 ** (exponent - 15)


def decode(fmt, data, offset):
    if fmt in (37, 43):
        return [data[offset + i] / 255.0 for i in range(4)], 4
    if fmt in (44, 50):
        b, g, r, a = data[offset:offset + 4]
        return [r / 255.0, g / 255.0, b / 255.0, a / 255.0], 4
    if fmt == 9:
        return [data[offset] / 255.0] * 3 + [1.0], 1
    if fmt == 16:
        return [data[offset] / 255.0, data[offset + 1] / 255.0, 0.0, 1.0], 2
    if fmt == 97:
        v = struct.unpack_from('<4H', data, offset)
        return [half(x) for x in v], 8
    if fmt == 83:
        v = struct.unpack_from('<2H', data, offset)
        return [half(v[0]), half(v[1]), 0.0, 1.0], 4
    if fmt == 76:
        v = half(struct.unpack_from('<H', data, offset)[0])
        return [v, v, v, 1.0], 2
    if fmt == 100:
        v = struct.unpack_from('<f', data, offset)[0]
        return [v, v, v, 1.0], 4
    if fmt == 109:
        return list(struct.unpack_from('<4f', data, offset)), 16
    if fmt == 103:
        v = struct.unpack_from('<2f', data, offset)
        return [v[0], v[1], 0.0, 1.0], 8
    if fmt == 122:
        w = struct.unpack_from('<I', data, offset)[0]
        return [small_float(w & 0x7ff, 6), small_float((w >> 11) & 0x7ff, 6), small_float(w >> 22, 5), 1.0], 4
    if fmt in (64, 58):
        w = struct.unpack_from('<I', data, offset)[0]
        r, g, b, a = w & 0x3ff, (w >> 10) & 0x3ff, (w >> 20) & 0x3ff, w >> 30
        if fmt == 58:
            r, b = b, r
        return [r / 1023.0, g / 1023.0, b / 1023.0, a / 3.0], 4
    if fmt == 98:
        w = struct.unpack_from('<I', data, offset)[0]
        return [small_float(w >> 21, 6), small_float((w >> 10) & 0x7ff, 6), small_float(w & 0x3ff, 5), 1.0], 4
    if fmt == 92:
        v = struct.unpack_from('<4h', data, offset)
        return [max(-1.0, x / 32767.0) * 0.5 + 0.5 for x in v[:3]] + [1.0], 8
    if fmt == 70:
        v = struct.unpack_from('<H', data, offset)[0] / 65535.0
        return [v, v, v, 1.0], 2
    return None, 0


rows = []
for path in sorted(glob.glob(os.path.join(src, '*.raw'))):
    data = open(path, 'rb').read()
    magic, width, height, pitch, fmt = struct.unpack_from('<5I', data, 0)
    if magic != 0x31474d49:
        continue
    body = memoryview(data)[20:]
    probe, size = decode(fmt, body, 0)
    name = os.path.basename(path)
    if probe is None:
        rows.append((name, width, height, fmt, 'unsupported format'))
        continue
    out_w = max(1, (width + step - 1) // step)
    out_h = max(1, (height + step - 1) // step)
    image = Image.new('RGB', (out_w, out_h))
    pixels = image.load()
    total = [0.0, 0.0, 0.0, 0.0]
    peak = 0.0
    nonzero = 0
    count = 0
    for y in range(0, height, step):
        base = y * pitch
        for x in range(0, width, step):
            offset = base + x * size
            if offset + size > len(body):
                continue
            value, _ = decode(fmt, body, offset)
            clean = [0.0 if (math.isnan(c) or math.isinf(c)) else c for c in value]
            for i in range(4):
                total[i] += clean[i]
            peak = max(peak, max(clean[:3]))
            nonzero += any(abs(c) > 1e-6 for c in clean[:3])
            count += 1
            pixels[x // step, y // step] = tuple(max(0, min(255, int(255 * (c / (1.0 + c) * 2.0 if fmt in (97, 109, 122, 83, 76, 100, 103, 98) else c)))) for c in clean[:3])
    image.save(os.path.join(dst, name.replace('.raw', '.png')))
    mean = [t / max(count, 1) for t in total]
    rows.append((name, width, height, fmt, f'mean rgba {mean[0]:.3f} {mean[1]:.3f} {mean[2]:.3f} {mean[3]:.3f} peak {peak:.3f} nonzero {nonzero / max(count, 1):.2f}'))
for row in rows:
    print(f'{row[0]:60} {row[1]:5}x{row[2]:<5} f{row[3]:<4} {row[4]}')
