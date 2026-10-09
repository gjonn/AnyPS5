import bisect
import csv
import os
import subprocess
import sys

csv_path, libs = sys.argv[1], sys.argv[2]
max_depth = int(sys.argv[3]) if len(sys.argv) > 3 else 14
min_seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 0.05
tables = {}


def table(module):
    if module in tables:
        return tables[module]
    path = os.path.join(libs, module)
    entries = []
    if os.path.exists(path):
        output = subprocess.run(['nm', '-C', '--defined-only', path], capture_output=True, text=True).stdout
        for line in output.splitlines():
            parts = line.split(' ', 2)
            if len(parts) == 3 and parts[1] in 'tT':
                entries.append((int(parts[0], 16), parts[2]))
        entries.sort()
    tables[module] = ([e[0] for e in entries], [e[1] for e in entries])
    return tables[module]


def resolve(module, start, fallback):
    keys, names = table(module)
    if keys and start.startswith('0x'):
        index = bisect.bisect_right(keys, int(start, 16)) - 1
        if index >= 0:
            return names[index]
    return fallback


with open(csv_path, encoding='utf-8-sig') as handle:
    reader = csv.reader(handle)
    header = next(reader)
    for row in reader:
        raw = row[0]
        depth = len(raw) - len(raw.lstrip(' '))
        seconds = float(row[1])
        if depth > max_depth or seconds < min_seconds:
            continue
        module, start = row[5], row[8]
        label = resolve(module, start, raw.strip())
        print(f"{'  ' * depth}{seconds:6.3f} {module[:14]:14} {label[:130]}")
