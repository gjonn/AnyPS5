import collections
import sqlite3
import sys

path = sys.argv[1]
top_threads = int(sys.argv[2]) if len(sys.argv) > 2 else 16
top_funcs = int(sys.argv[3]) if len(sys.argv) > 3 else 8
c = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
strings = dict(c.execute('select id, value from StringIds'))
pid = next(p for p, n in c.execute('select pid, name from PROCESSES') if (n or '').lower().endswith('app.exe') and not (n or '').lower().endswith('unsecapp.exe'))
names = {g: strings.get(n, '') for n, _, g in c.execute('select nameId, priority, globalTid from ThreadNames')}
per_thread = collections.Counter()
sample_thread = {}
for i, g in c.execute('select id, globalTid from COMPOSITE_EVENTS'):
    if (g >> 24) & 0xFFFFFF == pid:
        per_thread[g] += 1
        sample_thread[i] = g
chosen = {g for g, _ in per_thread.most_common(top_threads)}
leaf = collections.defaultdict(collections.Counter)
module_hits = collections.defaultdict(collections.Counter)
for i, sym, mod, depth in c.execute('select id, symbol, module, stackDepth from SAMPLING_CALLCHAINS where stackDepth < 40'):
    g = sample_thread.get(i)
    if g not in chosen:
        continue
    m = strings.get(mod, '?').replace('\\', '/').rsplit('/', 1)[-1]
    if depth == 0:
        leaf[g][f'{m}!{strings.get(sym, "?")[:90]}'] += 1
    module_hits[g][m] += 1
for g, n in per_thread.most_common(top_threads):
    mods = ', '.join(f'{m} {v}' for m, v in module_hits[g].most_common(5))
    print(f'\n== tid {g & 0xFFFFFF} {names.get(g, "")!r} samples {n} | frames by module: {mods}')
    for name, k in leaf[g].most_common(top_funcs):
        print(f'   {k:7} {100*k/n:5.1f}%  {name}')
