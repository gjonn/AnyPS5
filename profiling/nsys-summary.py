import collections
import sqlite3
import sys

path = sys.argv[1]
top_threads = int(sys.argv[2]) if len(sys.argv) > 2 else 12
c = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
strings = dict(c.execute('select id, value from StringIds'))
pid_of = lambda g: (g >> 24) & 0xFFFFFF
tid_of = lambda g: g & 0xFFFFFF
app = [r for r in c.execute('select pid, name from PROCESSES') if 'app.exe' in (r[1] or '').lower()]
pids = {p for p, _ in app}
print('app pids', app)
names = {g: strings.get(n, '') for n, _, g in c.execute('select nameId, priority, globalTid from ThreadNames')}
t0, t1 = c.execute('select min(start), max(start) from COMPOSITE_EVENTS').fetchone()
span = (t1 - t0) / 1e9
print(f'sample span {span:.2f} s')

running = collections.Counter()
blocked = collections.defaultdict(collections.Counter)
blocks = dict(c.execute('select id, name from ENUM_SCHEDULING_THREAD_BLOCK'))
last_in = {}
last_out = {}
waits = collections.defaultdict(list)
for start, cpu, is_in, g, state, block in c.execute('select start, cpu, isSchedIn, globalTid, threadState, threadBlock from SCHED_EVENTS order by start'):
    if pid_of(g) not in pids:
        continue
    if is_in:
        last_in[g] = start
        if g in last_out:
            out_start, reason = last_out.pop(g)
            blocked[g][reason] += start - out_start
            waits[g].append(start - out_start)
    else:
        if g in last_in:
            running[g] += start - last_in.pop(g)
        last_out[g] = (start, blocks.get(block, str(block)))

samples = collections.Counter(g for (g,) in c.execute('select globalTid from COMPOSITE_EVENTS') if pid_of(g) in pids)
print(f'\n{"tid":>6} {"run s":>6} {"samp":>6}  top block reasons (s)  name')
for g, ns in running.most_common(top_threads):
    reasons = ', '.join(f'{r} {v/1e9:.2f}' for r, v in blocked[g].most_common(3))
    w = sorted(waits[g])
    med = w[len(w) // 2] / 1e6 if w else 0
    print(f'{tid_of(g):>6} {ns/1e9:6.2f} {samples[g]:6}  [{reasons}] waits={len(w)} med={med:.3f}ms  {names.get(g, "")}')

print('\nGPU workloads')
work = sorted((s, e, strings.get(n, '')) for s, e, n in c.execute('select start, end, nameId from VULKAN_WORKLOAD'))
if work:
    busy = 0
    cur_s, cur_e = work[0][0], work[0][1]
    gaps = []
    for s, e, _ in work[1:]:
        if s > cur_e:
            busy += cur_e - cur_s
            gaps.append(s - cur_e)
            cur_s, cur_e = s, e
        else:
            cur_e = max(cur_e, e)
    busy += cur_e - cur_s
    wspan = work[-1][1] - work[0][0]
    gaps.sort()
    print(f'{len(work)} workloads over {wspan/1e9:.2f} s, busy {busy/1e9:.2f} s ({100*busy/wspan:.0f}%), gaps {len(gaps)} p50 {gaps[len(gaps)//2]/1e6:.2f} ms p90 {gaps[int(len(gaps)*.9)]/1e6:.2f} ms max {gaps[-1]/1e6:.1f} ms')
    durs = sorted(e - s for s, e, _ in work)
    print(f'workload dur p50 {durs[len(durs)//2]/1e6:.2f} ms p90 {durs[int(len(durs)*.9)]/1e6:.2f} ms max {durs[-1]/1e6:.1f} ms')

print('\nVulkan API by thread (total s, calls)')
api = collections.defaultdict(lambda: [0, 0])
for s, e, g, n in c.execute('select start, end, globalTid, nameId from VULKAN_API where end > start'):
    if pid_of(g) not in pids:
        continue
    k = (tid_of(g), strings.get(n, str(n)))
    api[k][0] += e - s
    api[k][1] += 1
for (tid, name), (ns, n) in sorted(api.items(), key=lambda kv: -kv[1][0])[:25]:
    print(f'{tid:>6} {name:40} {ns/1e9:7.3f} {n:7}')
