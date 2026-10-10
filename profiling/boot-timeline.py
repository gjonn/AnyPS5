import re
import sys
import json
from pathlib import Path

for path in sys.argv[1:]:
    present = []
    modules = []
    variants = []
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    for line in lines:
        m = re.match(r'^\[present\] (\d+) presents', line)
        if m:
            present.append(int(m.group(1)))
        m = re.match(r'^\[shader-disk-cache\] \(10 s\): (\d+) hits, (\d+) misses.*?specialized modules (\d+) hits, (\d+) misses', line)
        if m:
            variants.append((int(m.group(1)), int(m.group(2))))
            modules.append((int(m.group(3)), int(m.group(4))))
    steady = None
    for i in range(len(present) - 3):
        if all(40 <= p <= 200 for p in present[i:i + 4]) and max(present[:i] or [0]) >= 450:
            steady = i
            break
    print(f'== {path}')
    print('present windows:', ' '.join(map(str, present)))
    print('variant hits/misses per window:', ' '.join(f'{h}/{m}' for h, m in variants))
    print('module hits/misses per window:', ' '.join(f'{h}/{m}' for h, m in modules))
    if steady is not None:
        print(f'Presentation-rate-only candidate at window {steady}; this does not establish a loaded menu or elapsed wall time.')
    else:
        print('No sustained menu-like presentation rate detected.')

    root = Path(path).parent
    event_path = root / 'events.jsonl'
    if not event_path.exists():
        continue
    events = [json.loads(line) for line in event_path.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    misses = None
    cached = 0
    quiet = []
    first_window = None
    candidate = None
    summary = {'run': root.name, 'first_present_window_seconds': None, 'quiet_menu_candidate_seconds': None, 'confirmed_after_seconds': None, 'visual_confirmation_required': True}
    for event in events:
        line = event['line']
        elapsed = event['elapsedSeconds']
        pipe = re.match(r'^\[pipecache\].*? (\d+) misses,.*? (\d+) cached', line)
        if pipe:
            misses, cached = map(int, pipe.groups())
        frame = re.match(r'^\[present\] (\d+) presents', line)
        if frame:
            if first_window is None:
                first_window = elapsed
                summary['first_present_window_seconds'] = elapsed
            count = int(frame.group(1))
            if 20 <= count <= 200 and misses == 0 and cached >= 962:
                quiet.append(elapsed)
            else:
                quiet.clear()
            if candidate is None and len(quiet) == 4:
                candidate = quiet[0]
                summary['quiet_menu_candidate_seconds'] = candidate
                summary['confirmed_after_seconds'] = elapsed
    namespaces = re.findall(r'\(source version ([0-9a-f]+), format (\d+)\)', '\n'.join(lines))
    summary['cache_versions'] = sorted(set(namespaces))
    summary['logged_control_flow_seconds'] = sum(float(value) for value in re.findall(r'^\[control-flow\].*? ms=([0-9.]+)', '\n'.join(lines), re.MULTILINE)) / 1000
    if candidate is not None:
        summary['first_present_window_to_quiet_menu_seconds'] = candidate - first_window
    final_totals = re.findall(r'totals: (\d+) hits, (\d+) misses, (\d+) writes; modules (\d+) hits, (\d+) misses, (\d+) writes', '\n'.join(lines))
    if final_totals:
        summary['disk_cache_totals'] = dict(zip(('variant_hits','variant_misses','variant_writes','module_hits','module_misses','module_writes'), map(int, final_totals[-1])))
    print('Timestamped summary:', json.dumps(summary, indent=2))
    (root / 'timeline-summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
