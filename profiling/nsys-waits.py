import bisect
import collections
import os
import sqlite3
import subprocess
import sys

path = sys.argv[1]
tids = [int(x) for x in sys.argv[2].split(',')]
max_block_ms = float(sys.argv[3]) if len(sys.argv) > 3 else 100
depth_shown = int(sys.argv[4]) if len(sys.argv) > 4 else 7
c = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
s = dict(c.execute('select id, value from StringIds'))
pid = next(p for p, n in c.execute('select pid, name from PROCESSES') if (n or '').lower().endswith('\\app.exe') or (n or '').lower() == 'app.exe')
tables = {}


def table(module):
    if module not in tables:
        entries = []
        if module.lower().endswith(('.prx', '.dll', '.exe')) and os.path.exists(module) and 'windows' not in module.lower():
            out = subprocess.run(['nm', '-C', '--defined-only', module], capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split(' ', 2)
                if len(parts) == 3 and parts[1] in 'tT':
                    try:
                        entries.append((int(parts[0], 16), parts[2]))
                    except ValueError:
                        pass
            entries.sort()
        tables[module] = ([e[0] for e in entries], [e[1] for e in entries])
    return tables[module]


def name(module, ip, sym):
    short = module.replace(chr(92), '/').rsplit('/', 1)[-1]
    keys, names = table(module)
    if keys:
        i = bisect.bisect_right(keys, ip) - 1
        if i >= 0 and ip - keys[i] < 0x100000:
            return f'{short}!{names[i][:110]}'
    return f'{short}!{sym[:60]}'


presents = [a for a, n, g in c.execute('select start, nameId, globalTid from VULKAN_API order by start') if s.get(n) == 'vkQueuePresentKHR' and (g >> 24) & 0xFFFFFF == pid]
lo, hi = presents[0], presents[-1]
for tid in tids:
    g = (pid << 24) | tid
    g_candidates = [x for (x,) in c.execute('select distinct globalTid from SCHED_EVENTS where globalTid & 16777215 = ? and (globalTid >> 24) & 16777215 = ?', (tid, pid))]
    g = g_candidates[0]
    ev = list(c.execute('select start, isSchedIn from SCHED_EVENTS where globalTid = ? and start between ? and ? order by start', (g, lo, hi)))
    outs = []
    for (t, i), (t2, i2) in zip(ev, ev[1:]):
        if not i and i2:
            outs.append((t, t2 - t))
    out_times = [o[0] for o in outs]
    samples = list(c.execute('select id, start from COMPOSITE_EVENTS where globalTid = ? and start between ? and ? order by start', (g, lo, hi)))
    chosen = {}
    for sid, t in samples:
        j = bisect.bisect_left(out_times, t - 5000)
        if j < len(outs) and abs(outs[j][0] - t) <= 5000:
            chosen[sid] = outs[j][1]
    stacks = collections.defaultdict(list)
    ids = list(chosen)
    for k in range(0, len(ids), 900):
        part = ids[k:k + 900]
        q = f'select id, stackDepth, module, symbol, originalIP from SAMPLING_CALLCHAINS where id in ({",".join("?" * len(part))})'
        for sid, depth, mod, sym, ip in c.execute(q, part):
            stacks[sid].append((depth, s.get(mod, '?'), s.get(sym, '?'), ip or 0))
    agg = collections.Counter()
    count = collections.Counter()
    total_block = sum(d for _, d in outs if d < max_block_ms * 1e6)
    for sid, block in chosen.items():
        if block >= max_block_ms * 1e6:
            continue
        frames = [name(m, ip, sym) for _, m, sym, ip in sorted(stacks[sid])]
        frames = [f for f in frames if not f.startswith(('ntoskrnl', 'ntdll', 'KernelBase', 'libwinpthread', 'kernel32', 'ucrtbase', 'win32u'))]
        key = ' <- '.join(frames[:depth_shown])
        agg[key] += block
        count[key] += 1
    covered = sum(agg.values())
    print(f'\n=== tid {tid}: {len(outs)} switch-outs, off-CPU {total_block/1e6:.0f} ms (blocks < {max_block_ms} ms) in {(hi-lo)/1e9:.1f} s; stacks matched for {covered/1e6:.0f} ms')
    for key, ns in agg.most_common(12):
        print(f'  {ns/1e6:7.1f} ms {count[key]:6}x  {key}')
