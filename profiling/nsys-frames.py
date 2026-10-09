import bisect
import collections
import sqlite3
import sys

path = sys.argv[1]
c = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
s = dict(c.execute('select id, value from StringIds'))
pid = next(p for p, n in c.execute('select pid, name from PROCESSES') if (n or '').lower().endswith('\\app.exe') or (n or '').lower() == 'app.exe')
api = [(a, b, g & 0xFFFFFF, s.get(n, str(n)), cid) for a, b, g, n, cid in c.execute('select start, end, globalTid, nameId, correlationId from VULKAN_API order by start') if (g >> 24) & 0xFFFFFF == pid]
work = sorted((a, b, cid) for a, b, cid in c.execute('select start, end, correlationId from VULKAN_WORKLOAD'))
presents = [a for a, b, t, n, _ in api if n == 'vkQueuePresentKHR']
print(f'{len(api)} API calls, {len(work)} GPU workloads, {len(presents)} presents')
if len(presents) < 3:
    sys.exit()
gaps = sorted(b - a for a, b in zip(presents, presents[1:]))
print(f'present interval p50 {gaps[len(gaps)//2]/1e6:.1f} ms p90 {gaps[int(len(gaps)*.9)]/1e6:.1f} ms -> {1e9/gaps[len(gaps)//2]:.2f} fps')

def union(intervals, lo, hi):
    total = 0
    cur = None
    for a, b in intervals:
        a, b = max(a, lo), min(b, hi)
        if b <= a:
            continue
        if cur and a <= cur[1]:
            cur[1] = max(cur[1], b)
        else:
            if cur:
                total += cur[1] - cur[0]
            cur = [a, b]
    if cur:
        total += cur[1] - cur[0]
    return total

gpu = [(a, b) for a, b, _ in work]
lo, hi = presents[0], presents[-1]
span = hi - lo
print(f'window {span/1e9:.2f} s: GPU busy {union(gpu, lo, hi)/span*100:.1f}%')

threads = collections.defaultdict(list)
for a, b, t, n, _ in api:
    threads[t].append((a, b, n))
print('\nper-thread Vulkan activity in window (busy = union of call spans, calls, top calls by time)')
for t, calls in sorted(threads.items(), key=lambda kv: -len(kv[1]))[:10]:
    inwin = [(a, b, n) for a, b, n in calls if lo <= a < hi]
    if not inwin:
        continue
    by = collections.Counter()
    for a, b, n in inwin:
        by[n] += b - a
    first, last = inwin[0][0], inwin[-1][1]
    print(f'  tid {t:>6} calls {len(inwin):7} api-busy {union([(a, b) for a, b, _ in inwin], lo, hi)/1e6:8.1f} ms  ' + ', '.join(f'{n} {v/1e6:.1f}' for n, v in by.most_common(4)))

submits = [(a, b, t, cid) for a, b, t, n, cid in api if n in ('vkQueueSubmit', 'vkQueueSubmit2', 'vkQueueSubmit2KHR')]
wstart = [w[0] for w in work]
lat = []
for a, b, t, cid in submits:
    i = bisect.bisect_left(wstart, b)
    if i < len(work) and lo <= a < hi:
        lat.append(work[i][0] - b)
lat.sort()
if lat:
    print(f'\nsubmit end -> next GPU workload start: p50 {lat[len(lat)//2]/1e6:.2f} ms p90 {lat[int(len(lat)*.9)]/1e6:.2f} ms over {len(lat)} submits')

print('\nper-frame breakdown (first 12 frames): interval, GPU busy, GPU first->last, submits, cmd-record calls, longest GPU idle gap and what the CPU did then')
cmd_calls = [(a, b, t) for a, b, t, n, _ in api if n.startswith('vkCmd')]
cmd_start = [x[0] for x in cmd_calls]
sub_start = [x[0] for x in submits]
for f0, f1 in list(zip(presents, presents[1:]))[:12]:
    fw = [(max(a, f0), min(b, f1)) for a, b in gpu if b > f0 and a < f1]
    busy = union(fw, f0, f1)
    nsub = bisect.bisect_left(sub_start, f1) - bisect.bisect_left(sub_start, f0)
    ncmd = bisect.bisect_left(cmd_start, f1) - bisect.bisect_left(cmd_start, f0)
    fw.sort()
    idle = []
    prev = f0
    for a, b in fw:
        if a > prev:
            idle.append((a - prev, prev, a))
        prev = max(prev, b)
    if prev < f1:
        idle.append((f1 - prev, prev, f1))
    idle.sort(reverse=True)
    top = idle[0] if idle else (0, f0, f0)
    rec = bisect.bisect_left(cmd_start, top[2]) - bisect.bisect_left(cmd_start, top[1])
    gpu_first = (fw[0][0] - f0) / 1e6 if fw else -1
    gpu_last = (fw[-1][1] - f0) / 1e6 if fw else -1
    print(f'  {(f1-f0)/1e6:6.1f} ms  gpu {busy/1e6:5.1f} ms  gpu[{gpu_first:5.1f}..{gpu_last:6.1f}]  submits {nsub:3}  cmds {ncmd:6}  longest idle {top[0]/1e6:5.1f} ms at +{(top[1]-f0)/1e6:5.1f} ({rec} cmds recorded during it)')

waits = collections.defaultdict(lambda: [0, 0, 0])
for a, b, t, n, _ in api:
    if lo <= a < hi and ('Wait' in n or n in ('vkGetFenceStatus', 'vkGetSemaphoreCounterValue', 'vkGetQueryPoolResults')):
        w = waits[(t, n)]
        w[0] += b - a
        w[1] += 1
        w[2] = max(w[2], b - a)
print('\nwaits in window (tid, call, total ms, count, max ms)')
for (t, n), (tot, k, mx) in sorted(waits.items(), key=lambda kv: -kv[1][0])[:12]:
    print(f'  {t:>6} {n:32} {tot/1e6:8.1f} {k:6} {mx/1e6:7.2f}')
