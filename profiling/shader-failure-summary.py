import collections
import json
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8-sig", errors="replace")
registrations = collections.Counter()
for reason, stage in re.findall(r"left unprepared: (.*?) \[type=(\d+)", text):
    registrations[(reason, int(stage))] += 1
runtime = collections.Counter()
windows = 0
for line in text.splitlines():
    if not line.startswith("[skips] by reason (10 s):"):
        continue
    windows += 1
    for count, kind, reason in re.findall(r"(\d+)x \{(draw|dispatch): ([^}]+)\}", line):
        runtime[(kind, reason)] += int(count)
print(json.dumps({
    "registration_failures": [
        {"reason": reason, "type": stage, "logged_addresses": count}
        for (reason, stage), count in registrations.most_common()
    ],
    "runtime_skip_report_windows": windows,
    "runtime_skips": [
        {"kind": kind, "reason": reason, "reported_count": count}
        for (kind, reason), count in runtime.most_common()
    ],
    "limitations": "Registration logs deduplicate code addresses. Runtime windows report only their eight most frequent reasons and omit the final incomplete window. Missing entries do not prove absence."
}, indent=2))
