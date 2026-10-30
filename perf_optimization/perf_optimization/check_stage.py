import json
import sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent
stage=sys.argv[1]
base=ROOT/'results/v0_baseline/fixed_20s_n4'
current=ROOT/'results'/stage/'fixed_20s_n4'
keys=('result_sha256','fingerprint','route_bytes','sketch','lifetime')
rows=[]
for oldfile in sorted(base.glob('*/0/summary.json')):
    old=json.loads(oldfile.read_text())
    newfile=current/oldfile.relative_to(base)
    if not newfile.exists(): continue
    new=json.loads(newfile.read_text())
    if 'error' in old:
        assert old['error']==new.get('error'), (old['case'],old,new)
        continue
    for key in keys:
        assert old[key]==new.get(key),(old['case'],key,old[key],new.get(key))
    rows.append((old,new))
print('Exact full-result and numeric parity:',len(rows),'successful fixed plans')
for a,b in rows:
    print(a['case'],'eval speedup',round(a['evaluation_seconds']/b['evaluation_seconds'],3),
          'total speedup',round(a['wall_seconds']/b['wall_seconds'],3))
if rows:
    print('Aggregate eval speedup',sum(a['evaluation_seconds'] for a,b in rows)/sum(b['evaluation_seconds'] for a,b in rows))
