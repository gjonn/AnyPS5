import collections
import sqlite3
import sys

path = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
c = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
s = dict(c.execute('select id, value from StringIds'))
pid = next(p for p, n in c.execute('select pid, name from PROCESSES') if (n or '').lower().endswith('\\app.exe') or (n or '').lower() == 'app.exe')
names = {g: s.get(n, '') for n, _, g in c.execute('select nameId, priority, globalTid from ThreadNames')}
st = {}
for i, g in c.execute('select id, globalTid from COMPOSITE_EVENTS'):
    if (g >> 24) & 0xFFFFFF == pid:
        st[i] = g
user = collections.Counter()
mods = collections.defaultdict(collections.Counter)
for i, m, sym in c.execute('select id, module, symbol from SAMPLING_CALLCHAINS where stackDepth = 0'):
    g = st.get(i)
    if g is None:
        continue
    mn = s.get(m, '?').replace(chr(92), '/').rsplit('/', 1)[-1]
    if mn != 'ntoskrnl.exe':
        user[g] += 1
        mods[g][mn] += 1
for g, n in user.most_common(top):
    print(f"{g & 0xFFFFFF:>6} {n:7} {names.get(g, '')[:22]:22} " + ', '.join(f'{m} {v}' for m, v in mods[g].most_common(4)))
