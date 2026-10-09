import bisect
import collections
import csv
import os
import subprocess
import sys

csv_path, libs = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 20
focus = sys.argv[4] if len(sys.argv) > 4 else 'libSceAgcDriver.prx'
skip_prefixes = ('std::', 'operator new', 'operator delete', '__gnu_cxx::', 'void std::', 'std::_')
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


first_caller = collections.Counter()
first_real = collections.Counter()
stack = []
with open(csv_path, encoding='utf-8-sig') as handle:
    reader = csv.reader(handle)
    next(reader)
    for row in reader:
        raw = row[0]
        depth = len(raw) - len(raw.lstrip(' '))
        seconds = float(row[1])
        module, start = row[5], row[8]
        name = resolve(module, start, raw.strip())
        del stack[depth:]
        stack.append((module, name))
        if module != focus:
            continue
        if any(m == focus for m, _ in stack[:-1]):
            continue
        first_caller[name[:140]] += seconds
for name, seconds in first_caller.most_common(top):
    print(f"{seconds:6.3f} {name}")
