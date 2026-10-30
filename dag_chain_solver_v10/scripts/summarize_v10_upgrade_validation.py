"""Verify frozen v10.1 evidence without altering historical v10.0 reports."""
from collections import Counter
from datetime import datetime,timezone
import hashlib
import json
import platform
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


def read(p):return json.loads(p.read_text(encoding="utf-8"))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    root=Path(__file__).resolve().parents[1]
    out=root/"reports/upgrade"
    def evidence(p):return dict(path=p.relative_to(root).as_posix(),sha256=sha(p))
    result_path=out/"RESULTS_V10_1.json";result=read(result_path)
    assert result["completed"] and result["version"]=="0.10.1" and len(result["rows"])==80
    source=result["source_sha256"]
    for name,digest in source.items():assert sha(root/name)==digest,("source changed",name)
    runs={}
    for variant,meta in result["run_metadata"].items():
        path=root/meta["run"];assert sha(path)==meta["run_sha256"]
        run=read(path);assert run["completed"] and len(run["rows"])==40 and run["source_sha256"]==source
        assert sha(Path(meta["solver_path"]))==meta["solver_executable_sha256"]
        runs[variant]=run
    assert set(runs)=={"exact_pool","exact"}
    smoke=result["legacy_smoke"];assert smoke and smoke["plans_identical"]==4 and smoke["official_scored"]==4
    assert sha(Path(smoke["summary"]))==smoke["sha256"]
    legacy=read(Path(smoke["summary"]));assert legacy["completed"] and legacy["source_sha256"]==source
    binaries={}
    for compiler in ("msvc","mingw"):
        for filename in ("npu_sched.exe","npu_sched_experiment.exe","timing_diagnostics.exe"):
            path=root/f"build_{compiler}_upgrade/bin"/filename;binaries[path.relative_to(root).as_posix()]=sha(path)
    formal=root/"build_msvc_upgrade/bin/npu_sched.exe";diagnostic=root/"build_msvc_upgrade/bin/timing_diagnostics.exe"
    expected=set(re.findall(r"add_test\(NAME\s+(\w+)",(root/"CMakeLists.txt").read_text(encoding="utf-8")))
    ctests={}
    for compiler in ("msvc","mingw"):
        xml=out/f"ctest_{compiler}.xml";tree=ET.parse(xml).getroot();cases=tree.findall(".//testcase")
        assert len(cases)==len(expected) and {x.attrib["name"] for x in cases}==expected
        assert not any(x.find("failure") is not None or x.find("error") is not None or x.find("skipped") is not None for x in cases)
        assert tree.attrib.get("failures","0")=="0" and tree.attrib.get("skipped","0")=="0"
        log_path=root/f"build_{compiler}_upgrade/Testing/Temporary/LastTest.log"
        log=log_path.read_text(encoding="utf-8",errors="replace")
        assert log.count("Test Passed.")==len(expected) and "Test Failed." not in log
        commands=[Path(x) for x in re.findall(r'^Command: "([^"]+)"',log,re.MULTILINE)]
        assert len(commands)==len(expected) and all(log_path.stat().st_mtime>=x.stat().st_mtime for x in commands)
        ctests[compiler]=dict(passed=len(cases),tests=sorted(expected),junit=evidence(xml),log=evidence(log_path),
                             executable_sha256={x.relative_to(root).as_posix():sha(x) for x in commands})
    contracts_path=root/"runs/upgrade/contracts_checks.json";contracts=read(contracts_path)
    assert contracts["source_sha256"]==source and contracts["completed"]
    contract_rows=contracts["rows"];assert len(contract_rows)==3
    assert {x["name"] for x in contract_rows}=={"msvc_cli","mingw_cli","msvc_graph_26"}
    for row in contract_rows:
        assert row["exit_code"]==0 and row["command"]
        assert sha(root/row["solver_path"])==row["solver_sha256"]
        assert sha(root/row["checker_path"])==row["checker_sha256"]
        for kind in ("stdout","stderr"):assert sha(root/row[kind+"_log"])==row[kind+"_sha256"]
        text=(root/row["stdout_log"]).read_text(encoding="utf-8",errors="replace")
        assert ("26 graph contract checks passed" if row["name"]=="msvc_graph_26" else "CLI contract checks passed") in text
    synthetic_path=root/"runs/upgrade/synthetic/report.json";synthetic=read(synthetic_path)
    assert synthetic["completed"] and synthetic["passed"]==64 and synthetic["failed"]==0
    assert synthetic["solver_executable_sha256"]==sha(formal)
    for row in synthetic["rows"]:
        assert row["status"]=="ok" and row["repeat_plan_identical"]
        folder=synthetic_path.parent/f'{row["fixture"]}_n{row["cores"]}'
        assert sha(folder/"plan.json")==sha(folder/"repeat_plan.json")==row["plan_sha256"]
        assert sha(folder/"result.json")==row["result_sha256"]
        assert sha(synthetic_path.parent/"inputs"/(row["fixture"]+".json"))==row["input_sha256"]
    timing_path=root/"runs/upgrade/timing_diff/report.json";timing=read(timing_path)
    assert timing["completed"] and timing["passed"]==144 and timing["failed"]==0
    assert Counter(x["group"] for x in timing["rows"])==dict(public=40,synthetic=64,adversarial=16,collisions=4,precision=20)
    meta=timing["metadata"]
    assert meta["diagnostics_sha256"]==sha(diagnostic) and meta["solver_sha256"]==sha(formal)
    assert meta["checker_sha256"]==sha(root/"scripts/check_official_timing.py")
    for name,digest in meta["fixture_sources_sha256"].items():assert sha(root/name)==digest
    for name,digest in meta["source_sha256"].items():assert sha(root/name)==digest
    official={(x["case"],x["cores"]):x for x in runs["exact"]["rows"]}
    for row in timing["rows"]:
        assert row["status"]=="ok"
        for kind in ("cpp","official"):
            assert sha(timing_path.parent/row[kind+"_trace"])==row[kind+"_trace_sha256"]
        if row["group"]=="public":
            scored=official[row["case"],row["cores"]]
            assert row["valid"] and row["cycles"]==scored["cycles"]
            assert row["input_sha256"]==scored["input_sha256"] and row["plan_sha256"]==scored["plan_sha256"]
            assert row["spill_bytes"]==scored["data_movement_bytes"]["spill_added_copy_bytes"]
            assert row["route_bytes"]+row["spill_bytes"]==scored["data_movement_bytes"]["scheduled_copy_bytes"]
    history={}
    for version in ("v7","v8","v9"):
        manifest=read(root/f"reports/{version}_preservation_manifest.json");files=manifest.get("files",manifest)
        for name,digest in files.items():assert sha(root.parent/f"dag_chain_solver_{version}"/name)==digest,(version,name)
        history[version+"_unchanged_files"]=len(files)
    reference=read(root/"reports/rules_manifest.json")["files"]
    for name,digest in reference.items():assert sha(root/"references/hackathon_20261030"/name)==digest
    history["official_unchanged_files"]=len(reference)
    preserved_path=out/"v10_0_baseline_manifest.json";preserved=read(preserved_path)
    for name,digest in preserved["evidence_sha256"].items():assert sha(root/name)==digest,("v10.0 evidence",name)
    for item in preserved["binaries"].values():assert sha(root/item["saved"])==item["sha256"]
    old_sources={}
    for name,digest in preserved["source_sha256"].items():
        candidates=[root/name,*sorted((root/"backups").glob("*/"+name))]
        found=next((p for p in candidates if p.is_file() and sha(p)==digest),None)
        assert found is not None,("original v10.0 source missing from unchanged files/backups",name)
        old_sources[name]=evidence(found)
    history.update(v10_0_unchanged_evidence_files=len(preserved["evidence_sha256"]),v10_0_preserved_binaries=len(preserved["binaries"]),v10_0_reconstructible_source_files=len(old_sources))
    source_path=out/"SOURCE_V10_1.json"
    source_doc=dict(version="0.10.1",verified_utc=datetime.now(timezone.utc).isoformat(),source_sha256=source,solver_sha256=binaries,
                    baseline_manifest=evidence(preserved_path),v10_0_source_evidence=old_sources,
                    python_runtime=dict(executable=sys.executable,version=sys.version,platform=platform.platform()))
    source_path.write_text(json.dumps(source_doc,indent=2),encoding="utf-8")
    validation=dict(version="0.10.1",verified_utc=datetime.now(timezone.utc).isoformat(),source=evidence(source_path),cpp_tests=ctests,
                    cli_and_graph_contracts=dict(checks=contract_rows,report=evidence(contracts_path)),
                    synthetic=dict(passed=64,repeat_identity_passed=64,report=evidence(synthetic_path)),
                    exact_timing=dict(passed=144,by_group=timing["by_group"],matched_official_rejections=timing["matched_official_rejections"],report=evidence(timing_path),
                                      compared=["validity","cycles","route/spill bytes","local cycles","memory peak","pipe orders","full memory-dependency metadata","local/global op timelines"]),
                    official=dict(passed=80,legacy_smoke_identical=4,legacy_smoke_official_passed=4,report=evidence(result_path)),
                    preservation=history,limitations=["Finite differential cases are regression evidence, not universal proof.",
                        "Windows MSVC/MinGW tested; native Linux/older GCC not tested.",
                        "Historical v10.0 generation times may differ due to machine conditions.",
                        "tail_seconds is a component of exact_seconds, not an additional disjoint phase."])
    target=out/"VALIDATION_V10_1.json";target.write_text(json.dumps(validation,indent=2),encoding="utf-8")
    print(json.dumps({k:validation[k] for k in ("version","official","exact_timing","preservation")},indent=2))

if __name__=="__main__":main()
