import bisect
import csv
import os
import subprocess
import sys

csv_path, libs = sys.argv[1], sys.argv[2]
min_percent = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
max_depth = int(sys.argv[4]) if len(sys.argv) > 4 else 30
tables = {}


def table(module):
    if module not in tables:
        path = os.path.join(libs, module)
        entries = []
        if os.path.exists(path):
            out = subprocess.run(['nm', '-C', '--defined-only', path], capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split(' ', 2)
                if len(parts) == 3 and parts[1] in 'tT':
                    entries.append((int(parts[0], 16), parts[2]))
            entries.sort()
        tables[module] = ([e[0] for e in entries], [e[1] for e in entries])
    return tables[module]


def resolve(module, start, fallback):
    keys, names = table(module)
    if keys and start.startswith('0x'):
        i = bisect.bisect_right(keys, int(start, 16)) - 1
        if i >= 0:
            return names[i]
    return fallback


with open(csv_path, encoding='utf-8-sig') as handle:
    rows = list(csv.reader(handle))
start = next(i for i, r in enumerate(rows) if r and r[0] == 'Function Stack') + 1
collapse = []
for row in rows[start:]:
    raw = row[0]
    depth = len(raw) - len(raw.lstrip(' '))
    total, self_ = float(row[1]), float(row[2])
    if total < min_percent or depth > max_depth:
        continue
    name = resolve(row[3], row[4], raw.strip())
    if row[3] in ('ntdll.dll', 'KERNEL32.DLL', 'libwinpthread-1.dll') and depth < 6:
        continue
    print(f"{'  ' * min(depth, 40)}{total:5.1f}% (self {self_:4.1f}) {row[3][:12]:12} {name[:120]}")
