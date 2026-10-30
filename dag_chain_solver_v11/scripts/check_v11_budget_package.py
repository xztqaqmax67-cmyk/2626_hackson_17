"""Check the delivered source ZIP against the tested source and current docs."""
import hashlib
import json
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[1]
archive = root / 'dist/dag_chain_solver_v11_1.zip'
source_path = root / 'reports/budget20/SOURCE_V11_BUDGET20.json'
validation_path = root / 'reports/budget20/VALIDATION_V11_BUDGET20.json'
source = json.loads(source_path.read_text(encoding='utf-8'))
validation = json.loads(validation_path.read_text(encoding='utf-8'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


assert source['version'] == validation['version'] == '0.11.1'
assert validation['official']['passed'] == 40
for name, digest in source['source_sha256'].items():
    assert sha(root / name) == digest, ('tested source differs', name)
files = [root / name for name in ('CMakeLists.txt', 'README.md', 'SEARCH_LIMITS_zh.md', 'CHANGES_v11_zh.md')]
files += [p for p in sorted((root / 'src').glob('*')) if p.is_file()]
members = {'dag_chain_solver_v11_1/' + p.relative_to(root).as_posix(): p for p in files}
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    assert len(z.namelist()) == len(members) and set(z.namelist()) == set(members)
    for name, path in members.items():
        assert z.read(name) == path.read_bytes(), ('packaged bytes differ', name)
result = dict(version='0.11.1', archive=archive.relative_to(root).as_posix(),
              bytes=archive.stat().st_size, sha256=sha(archive), files=len(members),
              crc_ok=True, exact_file_set=True, tested_source_bytes_match=True,
              source_manifest_sha256=sha(source_path), validation_sha256=sha(validation_path),
              member_sha256={name: sha(path) for name, path in members.items()},
              note='Source-only package. Tests, official data, runs and reports stay in the working folder.')
target = root / 'reports/budget20/PACKAGE_V11_BUDGET20.json'
target.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps({k: result[k] for k in ('archive', 'bytes', 'sha256', 'files', 'tested_source_bytes_match')}))
