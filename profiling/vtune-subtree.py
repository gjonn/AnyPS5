import bisect
import collections
import csv
import os
import subprocess
import sys

csv_path, libs, focus = sys.argv[1], sys.argv[2], sys.argv[3]
levels = int(sys.argv[4]) if len(sys.argv) > 4 else 2
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
stack = []
focus_total = 0.0
children = [collections.Counter() for _ in range(levels)]
self_in = collections.Counter()
for row in rows[start:]:
    raw = ','.join(row[:-4])
    depth = len(raw) - len(raw.lstrip(' '))
    try:
        total, self_ = float(row[-4]), float(row[-3])
    except ValueError:
        continue
    name = resolve(row[-2], row[-1], raw.strip())
    del stack[depth:]
    stack.append(name)
    hits = [i for i, n in enumerate(stack[:-1]) if focus in n]
    if focus in name and not hits:
        focus_total += total
        continue
    if hits:
        rel = len(stack) - 1 - hits[0]
        if 1 <= rel <= levels:
            children[rel - 1][' > '.join(n[:70] for n in stack[hits[0] + 1:])] += total
        self_in[name[:110]] += self_
print(f'{focus}: {focus_total:.2f}% of all sampled CPU')
for level, counter in enumerate(children):
    print(f'\n-- level {level + 1} (inclusive % of all CPU)')
    for key, value in counter.most_common(18):
        if value >= 0.05:
            print(f'  {value:6.2f}  {key[:220]}')
print('\n-- self time inside the subtree')
for key, value in self_in.most_common(25):
    print(f'  {value:6.2f}  {key}')
