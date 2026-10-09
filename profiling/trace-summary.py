"""Summarize queue-worker idle intervals and overlapping guest wait reports.

These are correlations, not ownership or GPU-idleness proofs. In particular,
the usleep trace reports the requested sleep duration rather than elapsed time.
"""
import argparse
import collections
import json
import re
import statistics
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("log", type=Path)
parser.add_argument("--seconds", type=float, default=60)
parser.add_argument("--thread", type=int, action="append", default=[], help="Include all wait sites for this thread ID")
args = parser.parse_args()
events, waits, submissions = [], [], []
for line in args.log.read_text(errors="replace").splitlines():
    match = re.search(r"\[gpu\] ([\d.]+) (idle|execute|submit|done) .*?queue=0x([0-9a-f]+)", line)
    if match:
        events.append((float(match[1]), match[2], int(match[3], 16)))
    match = re.search(r"\[wt\] ([\d.]+) tid (\d+) (\S+) exe\+0x([0-9a-f]+) waited ([\d.]+) ms(.*)", line)
    if match:
        waits.append(dict(end=float(match[1]), tid=int(match[2]), kind=match[3],
                          caller=match[4], duration=float(match[5]), timeout="timeout" in match[6]))
    match = re.search(r"\[submit-return\] ([\d.]+) tid (\d+) queue=0x([0-9a-f]+) elapsed ([\d.]+) ms", line)
    if match:
        submissions.append(dict(end=float(match[1]), tid=int(match[2]), queue=int(match[3], 16), duration=float(match[4])))
latest = max([e[0] for e in events] + [w["end"] for w in waits] + [s["end"] for s in submissions], default=0)
cutoff = latest - args.seconds * 1000
events.sort()
idle, gaps = {}, []
for when, kind, queue in events:
    if kind == "idle":
        idle.setdefault(queue, when)
    elif kind == "execute" and queue in idle:
        start = idle.pop(queue)
        if start >= cutoff:
            gaps.append(dict(queue=hex(queue), start=start, end=when, ms=round(when-start, 3)))
summary = {}
for queue in sorted({g["queue"] for g in gaps}):
    values = [g["ms"] for g in gaps if g["queue"] == queue]
    summary[queue] = dict(count=len(values), medianMs=statistics.median(values), maxMs=max(values))
sites = collections.defaultdict(lambda: [0, 0., 0])
for wait in waits:
    if wait["end"] >= cutoff:
        key = f'{wait["tid"]} {wait["kind"]} exe+0x{wait["caller"]}'
        sites[key][0] += 1
        sites[key][1] += min(wait["duration"], wait["end"] - cutoff)
        sites[key][2] += wait["timeout"]
submit_groups = collections.defaultdict(list)
for submission in submissions:
    if submission["end"] >= cutoff:
        submit_groups[(submission["tid"], submission["queue"])].append(submission["duration"])
submit_summary = [dict(tid=tid, queue=hex(queue), count=len(values),
                       medianMs=statistics.median(values), maxMs=max(values))
                  for (tid, queue), values in sorted(submit_groups.items())]
submit_tids = {tid for tid, _ in submit_groups}
submit_waits = {key: value for key, value in sites.items() if int(key.split()[0]) in submit_tids}
largest = sorted((g for g in gaps if g["queue"] == "0x0"), key=lambda g: g["ms"], reverse=True)[:5]
for gap in largest:
    overlap = collections.defaultdict(float)
    for wait in waits:
        amount = min(gap["end"], wait["end"]) - max(gap["start"], wait["end"]-wait["duration"])
        if amount > 0:
            key = f'{wait["tid"]} {wait["kind"]} exe+0x{wait["caller"]}'
            overlap[key] += amount
    gap["overlappingReportedWaitMs"] = sorted(overlap.items(), key=lambda p: p[1], reverse=True)[:8]
print(json.dumps(dict(latestTraceMs=latest, windowSeconds=args.seconds,
                     queueWorkerIdleToExecute=summary, largestQueueZeroGaps=largest,
                     submissionApi=submit_summary, submittingThreadWaitSites=submit_waits,
                     selectedThreadWaitSites={key: value for key, value in sites.items() if int(key.split()[0]) in args.thread},
                     waitSites=sorted(sites.items(), key=lambda p: p[1][1], reverse=True)[:15],
                     caveat="Wait-site values: completed count, reported overlap milliseconds within window, timeout count. Pending waits are absent. Queue-worker idle is not GPU idle; overlap does not prove a dependency. usleep durations are requested."), indent=2))
