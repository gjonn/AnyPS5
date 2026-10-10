import csv
import json
import re
import statistics
import sys
from datetime import datetime
from pathlib import Path

root = Path(sys.argv[1])
started = datetime.fromisoformat(json.loads((root / 'process.json').read_text(encoding='utf-8-sig'))['started'])
events = [json.loads(line) for line in (root / 'events.jsonl').read_text(encoding='utf-8-sig').splitlines() if line.strip()]
samples = [json.loads(line) for line in (root / 'samples.jsonl').read_text(encoding='utf-8-sig').splitlines() if line.strip()]
transitions = []
initial = root / 'initial-controls.txt'
if initial.exists():
    match = re.search(r'^in_place_draw_inputs=(\d)$', initial.read_text(encoding='utf-8-sig'), re.M)
    if match:
        transitions.append((0, int(match[1])))
for event in events:
    match = re.match(r'^\[perf-controls\].*in_place_draw_inputs=(\d)', event['line'])
    if match:
        transitions.append((event['elapsedSeconds'], int(match[1])))
with (root / 'ab-segments.csv').open(encoding='utf-8-sig', newline='') as stream:
    segments = list(csv.DictReader(stream))
results = []
for segment in segments:
    begin = (datetime.fromisoformat(segment['start']) - started).total_seconds()
    end = (datetime.fromisoformat(segment['end']) - started).total_seconds()
    enabled = int(segment['enabled'])
    acknowledged = next((value for time, value in reversed(transitions) if time <= begin + 5), None)
    if acknowledged != enabled:
        raise RuntimeError(f"Segment {segment['name']} has no matching control acknowledgement")
    fps, gpu, vertex, draw_us, skipped = [], [], [], [], []
    pipeline_misses = 0
    previous_present = None
    for event in events:
        elapsed, line = event['elapsedSeconds'], event['line']
        present = re.match(r'^\[present\] (\d+) presents', line)
        interval = None if previous_present is None else elapsed - previous_present
        if present:
            previous_present = elapsed
        if not begin + 20 <= elapsed <= end:
            continue
        if present and interval and interval > 0:
            fps.append(int(present[1]) / interval)
            busy = re.search(r'GPU busy ([\d.]+) ms', line)
            if busy:
                gpu.append(float(busy[1]))
        draws = re.match(r'^\[draws\] (\d+) draws.*?recorded avg (\d+) us', line)
        if draws and int(draws[1]):
            draw_us.append(float(draws[2]))
            phase = re.search(r' vertex=([\d.]+)ms', line)
            if phase:
                vertex.append(float(phase[1]) * 1000 / int(draws[1]))
            thrown = re.search(r' thrown (\d+) in', line)
            if thrown:
                skipped.append(int(thrown[1]) / int(draws[1]))
        miss = re.match(r'^\[pipecache\].*? (\d+) misses', line)
        if miss:
            pipeline_misses += int(miss[1])
    median = lambda values: statistics.median(values) if values else None
    flips = []
    for sample in samples:
        frame = re.search(r'FPS: [\d.]+ \((\d+)\)', sample['title'])
        if frame and begin + 20 <= sample['elapsedSeconds'] <= end:
            flips.append((sample['elapsedSeconds'], int(frame[1])))
    flip_fps = (flips[-1][1] - flips[0][1]) / (flips[-1][0] - flips[0][0]) if len(flips) > 1 else None
    results.append(dict(name=segment['name'], enabled=enabled, start_seconds=begin, end_seconds=end,
                        present_windows=len(fps), fps_median=median(fps), guest_flip_fps=flip_fps, gpu_ms_median=median(gpu),
                        vertex_us_per_draw_median=median(vertex), recorded_draw_us_median=median(draw_us),
                        thrown_per_draw_median=median(skipped), pipeline_misses=pipeline_misses))
print(json.dumps(results, indent=2))
(root / 'ab-summary.json').write_text(json.dumps(results, indent=2) + '\n')
