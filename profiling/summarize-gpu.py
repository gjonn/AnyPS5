"""Rank the last complete GPU timing reports; these ranges are not GPU utilization."""
import argparse
import collections
import json
import re

parser = argparse.ArgumentParser()
parser.add_argument("log")
parser.add_argument("--last", type=int, default=3)
parser.add_argument("--out")
args = parser.parse_args()
if args.last < 1:
    parser.error("--last must be positive")

reports = collections.deque(maxlen=args.last)
header = re.compile(r"\[gputime\] ([\d.]+) ms of GPU time in (\d+) batches over 10 s \(batch ([\d.]+) ms.*?; (\d+) presents; (\d+) ranges dropped")
classes = re.compile(r"([\w-]+) x(\d+) ([\d.]+)ms ([\d.]+)MiB \(per present x[\d.]+ [\d.]+ms [\d.]+MiB\)")
programs = re.compile(r"(0x[0-9a-f]+) x(\d+) ([\d.]+)ms")
with open(args.log, encoding="utf-8", errors="replace") as source:
    for line in source:
        if not line.startswith("[gputime]") or "; by class:" not in line:
            continue
        match = header.search(line)
        if not match:
            continue
        program_part, class_part = line.split("; by class:", 1)
        reports.append({"presents": int(match[4]), "batches": int(match[2]),
                        "batch_ms": float(match[3]), "dropped_ranges": int(match[5]),
                        "classes": classes.findall(class_part), "programs": programs.findall(program_part)})

count = sum(report["presents"] for report in reports)
class_totals = collections.defaultdict(lambda: [0, 0.0, 0.0])
program_totals = collections.defaultdict(lambda: [0, 0.0])
for report in reports:
    for name, calls, ms, mib in report["classes"]:
        totals = class_totals[name]
        totals[0] += int(calls)
        totals[1] += float(ms)
        totals[2] += float(mib)
    for name, calls, ms in report["programs"]:
        program_totals[name][0] += int(calls)
        program_totals[name][1] += float(ms)

def per_frame(value):
    return round(value / count, 3) if count else None

result = {"reports": len(reports), "presents": count,
          "dropped_ranges": sum(report["dropped_ranges"] for report in reports),
          "batch_ms_per_present": per_frame(sum(report["batch_ms"] for report in reports)),
          "classes": [{"name": name, "ms_per_present": per_frame(totals[1]),
                       "calls_per_present": per_frame(totals[0]), "MiB_per_present": per_frame(totals[2])}
                      for name, totals in sorted(class_totals.items(), key=lambda item: -item[1][1])],
          "top_programs": [{"address": name, "ms_per_present": per_frame(totals[1]),
                            "calls_per_present": per_frame(totals[0])}
                           for name, totals in sorted(program_totals.items(), key=lambda item: -item[1][1])[:12]],
          "limits": "Selected reports can include loading. Program and class ranges may overlap; their sums are not physical GPU utilization. Per-present values mix completion/report boundaries."}
encoded = json.dumps(result, indent=2)
if args.out:
    with open(args.out, "w", encoding="utf-8") as output:
        output.write(encoded + "\n")
print(encoded)
