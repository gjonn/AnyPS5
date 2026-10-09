import bisect
import collections
import csv
import os
import re
import subprocess
import sys

csv_path, libs = sys.argv[1], sys.argv[2]
top_threads = int(sys.argv[3]) if len(sys.argv) > 3 else 8
top_functions = int(sys.argv[4]) if len(sys.argv) > 4 else 12
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


def name(module, address, fallback):
    keys, names = table(module)
    if keys:
        index = bisect.bisect_right(keys, address) - 1
        if index >= 0:
            return names[index]
    return fallback


rows = list(csv.DictReader(open(csv_path, encoding='utf-8-sig')))
per_thread = collections.defaultdict(list)
for row in rows:
    per_thread[row['Thread']].append(row)
totals = sorted(((sum(float(r['CPU Time']) for r in v), t) for t, v in per_thread.items()), reverse=True)
grand = sum(t for t, _ in totals)
print(f"total CPU {grand:.2f} s")
for total, thread in totals[:top_threads]:
    print(f"== {thread}: {total:.2f} s")
    for row in sorted(per_thread[thread], key=lambda r: -float(r['CPU Time']))[:top_functions]:
        start = row['Start Address']
        address = int(start, 16) if start.startswith('0x') else 0
        print(f"   {float(row['CPU Time']):6.3f} {row['Module'][:20]:20} {name(row['Module'], address, row['Function'])[:120]}")
