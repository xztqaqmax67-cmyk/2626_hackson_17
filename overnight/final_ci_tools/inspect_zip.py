"""Verify the final archive against its manifest before clean CI extraction.

This checks only the submitted ZIP, never uses a source checkout as build input,
and does not import an official evaluator or copy any benchmark data.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
import zipfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(content):
    return hashlib.sha256(content).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--destination", type=Path, default=Path("ci-unpacked"))
    parser.add_argument("--report", type=Path, default=Path("ci-output/package-inspection.json"))
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    archive_path = args.archive.resolve(strict=True)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    raw = archive_path.read_bytes()
    require(archive_path.name == "17.zip", "Archive must be named 17.zip")
    require(len(raw) <= 20 * 1024**2, "Archive exceeds 20 MiB")
    require(manifest["team"] == "17" and manifest["version"] == "0.8.3", "Wrong team/version manifest")
    require(sha(raw) == manifest["archive_sha256"], "ZIP hash differs from manifest")
    require(len(raw) == manifest["archive_bytes"], "ZIP byte count differs from manifest")
    files = {}
    with zipfile.ZipFile(archive_path) as archive:
        require(archive.testzip() is None, "ZIP CRC failure")
        for info in archive.infolist():
            name = info.filename
            path = PurePosixPath(name)
            require(not info.is_dir(), "Only explicit file entries are expected")
            require(not path.is_absolute() and path.parts[0] == "17" and ".." not in path.parts,
                    "Archive entry escaped 17/: " + name)
            require("\\" not in name and ":" not in name, "Unsafe archive path: " + name)
            require(name not in files, "Duplicate archive entry: " + name)
            require(not info.flag_bits & 1, "Encrypted archive entry: " + name)
            require(not stat.S_ISLNK(info.external_attr >> 16), "Archive symlink: " + name)
            relative = path.relative_to("17").as_posix()
            allowed = relative in {"CMakeLists.txt", "README.md"} or (
                path.parts[1] == "src" and path.suffix in {".cpp", ".hpp", ".h"})
            require(allowed, "Entry outside source-only allowlist: " + name)
            forbidden = {"build", "tests", "scripts", "data", "references", "official_code",
                         "official_evaluator", "answers", ".git", ".vs", ".vscode", "__pycache__"}
            require(not set(path.parts) & forbidden, "Forbidden archive directory: " + name)
            require(path.stem not in {"official_spill", "spill_feedback"}, "Official port entered ZIP")
            content = archive.read(info)
            content.decode("utf-8-sig")
            files[name] = content
    expected = {"17/" + name: metadata for name, metadata in manifest["package_files"].items()}
    require(set(files) == set(expected), "ZIP file list differs from manifest")
    require({"17/CMakeLists.txt", "17/README.md", "17/src/main.cpp"} <= set(files), "Missing required entry")
    for name, content in files.items():
        require(len(content) == expected[name]["bytes"] and sha(content) == expected[name]["sha256"],
                "Source identity mismatch: " + name)
    main_source = files["17/src/main.cpp"].decode("utf-8-sig")
    require("dag-chain-solver 0.8.3\\n" in main_source, "Wrong source version")
    report = dict(team="17", version="0.8.3", archive_sha256=sha(raw), archive_bytes=len(raw),
                  file_count=len(files), manifest_matches=True, clean_extraction=not args.check_only,
                  source_sha256={name: sha(content) for name, content in sorted(files.items())})
    if not args.check_only:
        destination = args.destination.resolve()
        require(not destination.exists(), "Clean extraction destination already exists")
        destination.mkdir(parents=True, exist_ok=False)
        for name, content in sorted(files.items()):
            target = destination / name
            require(target.resolve().is_relative_to(destination), "Extraction escaped destination")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
