"""Back up only explicitly listed files before a v10 edit; never copy directories."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('label')
parser.add_argument('files', nargs='+')
args = parser.parse_args()
assert args.label and all(c.isalnum() or c in '_-' for c in args.label)
root = Path(__file__).resolve().parents[1]
stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S_%fZ')
target = root/'backups'/(stamp+'_'+args.label)
rows = {}
for name in dict.fromkeys(args.files):
    file = (root/name).resolve()
    relative = file.relative_to(root)
    assert relative.parts[0] not in ('backups','references','build_msvc','build_mingw','runs'), name
    assert not file.is_dir(), 'List individual changed files, never a directory'
    if file.exists():
        destination = target/relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(file, destination)
        rows[relative.as_posix()] = {'existed':True,'sha256':hashlib.sha256(file.read_bytes()).hexdigest(),'bytes':file.stat().st_size}
    else:
        rows[relative.as_posix()] = {'existed':False}
target.mkdir(parents=True,exist_ok=True)
(target/'manifest.json').write_text(json.dumps({'label':args.label,'created_utc':stamp,'files':rows},indent=2),encoding='utf-8')
print(target)
