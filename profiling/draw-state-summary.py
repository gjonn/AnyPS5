import argparse
import json
from pathlib import Path


def summarize(path):
    lines = path.read_text().splitlines()
    if not lines or lines[0] != 'APS5_DRAW_STATE_1':
        raise ValueError(f'{path}: invalid header')
    banks = {name: {} for name in 'csu'}
    for row in lines[1:]:
        bank, offset, value = row.split()
        banks[bank][int(offset, 16)] = int(value, 16)
    cx = banks['c']
    targets = []
    for slot in range(8):
        info = cx.get(0x31c + slot * 15)
        if info is None or ((info >> 2) & 31) == 0:
            continue
        blend = cx.get(0x1e0 + slot, 0)
        alpha = blend >> 16 if blend & 0x20000000 else blend
        attrib2 = cx.get(0x3b0 + slot, 0)
        targets.append({
            'slot': slot,
            'info_hex': hex(info),
            'format': (info >> 2) & 31,
            'number_type': (info >> 8) & 7,
            'component_swap': (info >> 11) & 3,
            'target_write_mask': (cx.get(0x8e, 0) >> (4 * slot)) & 15,
            'shader_write_mask': (cx.get(0x8f, 0) >> (4 * slot)) & 15,
            'blend_enabled': bool(blend & 0x40000000),
            'blend_hex': hex(blend),
            'rgb': {'source_factor': blend & 31, 'destination_factor': (blend >> 8) & 31, 'operation': (blend >> 5) & 7},
            'alpha': {'source_factor': alpha & 31, 'destination_factor': (alpha >> 8) & 31, 'operation': (alpha >> 5) & 7},
            'dcc_enabled': bool(info & 0x10000000),
            'cmask_fast_clear': bool(info & 0x2000),
            'width': ((attrib2 >> 14) & 0x3fff) + 1,
            'height': (attrib2 & 0x3fff) + 1,
            'view_hex': hex(cx.get(0x31b + slot * 15, 0)),
            'attrib3_hex': hex(cx.get(0x3b8 + slot, 0)),
        })
    return {'file': path.name, 'targets': targets}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('path', type=Path)
    args = parser.parse_args()
    files = sorted(args.path.glob('draw-state-*.txt')) if args.path.is_dir() else [args.path]
    print(json.dumps([summarize(path) for path in files], indent=2))
