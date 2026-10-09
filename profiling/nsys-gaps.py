import bisect
import collections
import sqlite3
import sys

path = sys.argv[1]
long_gap_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 100
c = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
s = dict(c.execute('select id, value from StringIds'))
pid = next(p for p, n in c.execute('select pid, name from PROCESSES') if (n or '').lower().endswith('\\app.exe') or (n or '').lower() == 'app.exe')
names = {g & 0xFFFFFF: s.get(n, '') for n, _, g in c.execute('select nameId, priority, globalTid from ThreadNames') if (g >> 24) & 0xFFFFFF == pid}
work = sorted((a, b) for a, b in c.execute('select start, end from VULKAN_WORKLOAD'))
presents = [a for a, n, g in c.execute('select start, nameId, globalTid from VULKAN_API order by start') if s.get(n) == 'vkQueuePresentKHR' and (g >> 24) & 0xFFFFFF == pid]
lo, hi = presents[0], presents[-1]
gaps = []
prev = lo
for a, b in work:
    if b <= lo or a >= hi:
        continue
    if a > prev:
        gaps.append((prev, a))
    prev = max(prev, b)
long_gaps = [g for g in gaps if g[1] - g[0] >= long_gap_ms * 1e6]
short_gaps = [g for g in gaps if 0.2e6 <= g[1] - g[0] < long_gap_ms * 1e6]
busy = []
prev = None
for a, b in work:
    if b <= lo or a >= hi:
        continue
    busy.append((max(a, lo), min(b, hi)))
print(f'window {(hi-lo)/1e9:.2f} s; {len(long_gaps)} long GPU gaps (>= {long_gap_ms} ms) total {sum(b-a for a,b in long_gaps)/1e9:.2f} s; {len(short_gaps)} short gaps total {sum(b-a for a,b in short_gaps)/1e9:.2f} s')

running = collections.defaultdict(list)
state = {}
blocks = dict(c.execute('select id, name from ENUM_SCHEDULING_THREAD_BLOCK'))
for t, is_in, g in c.execute('select start, isSchedIn, globalTid from SCHED_EVENTS order by start'):
    if (g >> 24) & 0xFFFFFF != pid:
        continue
    tid = g & 0xFFFFFF
    if is_in:
        state[tid] = t
    elif tid in state:
        running[tid].append((state.pop(tid), t))

def overlap(intervals, spans):
    total = 0
    starts = [x[0] for x in intervals]
    for a, b in spans:
        i = max(0, bisect.bisect_left(starts, a) - 1)
        while i < len(intervals) and intervals[i][0] < b:
            x, y = intervals[i]
            total += max(0, min(y, b) - max(x, a))
            i += 1
    return total

def report(label, spans, top=14):
    dur = sum(b - a for a, b in spans)
    if not dur:
        return
    rows = sorted(((overlap(iv, spans), tid) for tid, iv in running.items()), reverse=True)[:top]
    print(f'\n{label}: {dur/1e6:.0f} ms total; thread running fraction during it')
    for ns, tid in rows:
        print(f'  {tid:>6} {100*ns/dur:5.1f}%  {names.get(tid, "")}')

report('long GPU gaps', long_gaps)
report('short GPU gaps', short_gaps)
report('GPU busy', busy)
if long_gaps:
    a, b = long_gaps[0]
    print(f'\nfirst long gap at +{(a-lo)/1e6:.0f} ms lasting {(b-a)/1e6:.0f} ms')
