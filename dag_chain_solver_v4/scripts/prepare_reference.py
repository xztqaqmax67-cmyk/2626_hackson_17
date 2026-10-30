"""Extract the pinned official package for development; never ship it in the submission."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import zipfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive', type=Path, default=root.parent/'dag_chain_solver_v2/hackathon.zip')
    p.add_argument('--out', type=Path, default=root/'references/hackathon_20261030')
    a = p.parse_args()
    manifest = json.loads((root/'reports/rules_manifest.json').read_text(encoding='utf-8'))
    assert sha(a.archive.read_bytes()) == manifest['supplied_archive_sha256'], 'official archive changed'
    target = a.out.resolve()
    target.mkdir(parents=True, exist_ok=True)
    count = 0
    with zipfile.ZipFile(a.archive) as archive:
        for name, digest in manifest['files'].items():
            relative = PurePosixPath(name)
            assert not relative.is_absolute() and '..' not in relative.parts
            destination = target.joinpath(*relative.parts)
            assert destination.resolve().is_relative_to(target), 'unsafe archive destination'
            data = archive.read('hackathon/' + name)
            assert sha(data) == digest, name
            if destination.exists():
                assert destination.read_bytes() == data, ('refuse to overwrite modified reference', name)
            else:
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
            count += 1
    print(f'{count} unmodified reference files ready at {target}')


if __name__ == '__main__':
    main()
