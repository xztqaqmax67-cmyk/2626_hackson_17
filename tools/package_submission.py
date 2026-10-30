"""Create exactly <team-id>.zip from a standalone solution-generator project."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--variant',choices=['old','new'],required=True)
parser.add_argument('--team-id',required=True)
parser.add_argument('--output-dir',type=Path,required=True)
args=parser.parse_args()
if not re.fullmatch(r'[A-Za-z0-9_-]+',args.team_id):
    raise SystemExit('team-id must contain only letters, digits, underscores or hyphens')
source=ROOT/('hackathon_v3' if args.variant=='old' else '前辈代码_cpp')/'submission'
files=[source/'CMakeLists.txt',source/'README.md']
for directory in ('src','include','third_party'):
    if (source/directory).exists(): files.extend(p for p in (source/directory).rglob('*') if p.is_file())
for path in files:
    if path.is_symlink() or not path.is_file():raise SystemExit('Missing file or unsupported link: '+str(path))
    if path.suffix.lower() in ('.exe','.dll','.o','.obj','.so','.a','.lib','.pdb','.pyc'):
        raise SystemExit('Build artifact in source directory: '+str(path))
args.output_dir.mkdir(parents=True,exist_ok=True)
target=args.output_dir/(args.team_id+'.zip')
if target.exists():raise SystemExit('Output already exists; choose a fresh directory: '+str(target))
inventory=[]
with zipfile.ZipFile(target,'x',zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(files):
        relative=path.relative_to(source).as_posix()
        archive.write(path,args.team_id+'/'+relative)
        inventory.append(dict(path=relative,bytes=path.stat().st_size,
                              sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
with zipfile.ZipFile(target) as archive:
    assert archive.testzip() is None
manifest=dict(variant=args.variant,team_id=args.team_id,zip_bytes=target.stat().st_size,
              zip_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),files=inventory)
(args.output_dir/(args.team_id+'_manifest.json')).write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print(str(target),flush=True)
