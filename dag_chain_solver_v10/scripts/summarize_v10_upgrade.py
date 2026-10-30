"""Compare v10.1 pool/tail runs with immutable v10.0 exact40 evidence.

Only writes reports/upgrade by default. Historical runs/reports are never changed.
"""
import argparse
import hashlib
import json
import statistics
from pathlib import Path


def read(p):return json.loads(p.read_text(encoding="utf-8"))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def change(a,b):return 100*(a/b-1) if b else None


def validate_row(root,row):
    assert row["status"]=="ok"
    assert sha(root/"references/hackathon_20261030/data"/(row["case"]+".json"))==row["input_sha256"]
    for field in ("plan","result"):assert sha(root/row[field])==row[field+"_sha256"]
    result=read(root/row["result"]);assert result["makespan"]==row["cycles"]
    assert result["num_cores"]==row["cores"] and result["data_movement_bytes"]==row["data_movement_bytes"]
    m=row["data_movement_bytes"]
    assert m["scheduled_copy_bytes"]==sum(m[k] for k in ("original_graph_copy_bytes","partition_added_copy_bytes","spill_added_copy_bytes"))
    assert row["speedup"]==row["singlecore_cycles"]/row["cycles"]
    return read(root/row["plan"])


def mapping(plan):return {op:c for c,stream in enumerate(plan["core_schedules"]) for op in stream}


def summarize(rows):
    out=dict(count=len(rows),mean_speedup=statistics.mean(x["speedup"] for x in rows),
             old_mean_speedup=statistics.mean(x["old_speedup"] for x in rows),
             generation_mean_seconds=statistics.mean(x["generation_seconds"] for x in rows),
             old_generation_mean_seconds=statistics.mean(x["old_generation_seconds"] for x in rows),
             generation_min_seconds=min(x["generation_seconds"] for x in rows),
             generation_max_seconds=max(x["generation_seconds"] for x in rows),
             changed_plans=sum(x["plan_changed"] for x in rows),changed_mappings=sum(x["mapping_changed"] for x in rows))
    out["mean_speedup_change_percent"]=change(out["mean_speedup"],out["old_mean_speedup"])
    for name in ("cycles","copy_bytes","spill_bytes","partition_bytes"):
        out[name]=sum(x[name] for x in rows);out["old_"+name]=sum(x["old_"+name] for x in rows)
        out[name+"_change_percent"]=change(out[name],out["old_"+name])
        out[name+"_improved"]=sum(x[name]<x["old_"+name] for x in rows)
        out[name+"_equal"]=sum(x[name]==x["old_"+name] for x in rows)
        out[name+"_regressed"]=sum(x[name]>x["old_"+name] for x in rows)
    stats=[x["diagnostics"]["v10_stats"] for x in rows]
    out["phase_mean_seconds"]={name:statistics.mean(x[name] for x in stats)
                               for name in ("candidate_seconds","feedback_seconds","exact_seconds","tail_seconds")}
    out["work_totals"]={name:sum(x[name] for x in stats) for name in (
        "generated","unique","traced","refined","feedback_starts","feedback_replays","feedback_refinements", 
        "exact_evaluations","exact_invalid","tail_generated","tail_evaluations","tail_duplicates","tail_accepted")}
    out["exact_rejection_reasons"]=[dict(case=x["case"],cores=x["cores"],errors=x["diagnostics"]["v10_stats"].get("exact_errors",[]))
                                    for x in rows if x["diagnostics"]["v10_stats"]["exact_invalid"]]
    return out


def markdown(path,rows,variants,incremental,tail_origin,final_only=False):
    selected=("exact",) if final_only else ("exact_pool","exact")
    lines=["# v10.1 正式版40組結果" if final_only else "# v10.1 決賽候選池與尾段優化：80組官方比較","",
           "基準是保留的 v10.0 exact40。每個新方案獨立生成、使用未修改的官方評分器；單核加速比分母沿用同一官方單核基準。時間為完整生成程序秒數；搬運單位為 bytes。", "",
           "| 方案 | 平均加速比 | 相對v10.0 | cycles總變化 | COPY總變化 | 溢出總變化 | 平均生成秒 |",
           "|---|---:|---:|---:|---:|---:|---:|"]
    base=variants["exact"]["summary"]
    lines.append(f'| v10.0 | {base["old_mean_speedup"]:.6f}× | — | — | — | — | {base["old_generation_mean_seconds"]:.3f} |')
    for name in selected:
        s=variants[name]["summary"]
        lines.append(f'| {name} | {s["mean_speedup"]:.6f}× | {s["mean_speedup_change_percent"]:+.4f}% | {s["cycles_change_percent"]:+.4f}% | {s["copy_bytes_change_percent"]:+.4f}% | {s["spill_bytes_change_percent"]:+.4f}% | {s["generation_mean_seconds"]:.3f} |')
    z=incremental
    lines += ["",f'完整 exact 相對 exact_pool：平均加速比 {z["mean_speedup_change_percent"]:+.4f}%；cycles 改善 {z["cycles_improved"]}、相同 {z["cycles_equal"]}、退步 {z["cycles_regressed"]} 組。',
              "", "tail_seconds 已包含於 exact_seconds，不能再次相加；v10.0時間為先前測量，環境波動未排除。"]
    lines += ["", "尾段最終勝者依本次 exact_candidates 的實際 phase 審計分類，非依沿用label推測：", ""]
    for phase in ("timeline","spill_exact"):
        t=tail_origin[phase]
        lines.append(f'- {phase}：{t["selected_points"]}組；其中cycles改善{t["cycle_improved_points"]}組、僅搬運改善{t["traffic_only_points"]}組；相對pool共減少{t["gain_cycles"]:,} cycles。')
    for name in selected:
        for cores in (2,3,4,5):
            s=variants[name]["by_cores"][str(cores)]
            lines += ["",f"## {name}：{cores} 核","",f'平均加速比 {s["old_mean_speedup"]:.6f}× → {s["mean_speedup"]:.6f}×（{s["mean_speedup_change_percent"]:+.4f}%）。',"",
                      "| 用例 | cycles（v10.0→v10.1） | 單核加速比（舊→新） | 新總COPY | Δ總COPY | 新溢出COPY | Δ溢出COPY | 生成秒（舊→新） |",
                      "|---|---:|---:|---:|---:|---:|---:|---:|"]
            for x in rows:
                if x["variant"]!=name or x["cores"]!=cores:continue
                lines.append(f'| {x["case"]} | {x["old_cycles"]:,} → {x["cycles"]:,} | {x["old_speedup"]:.6f} → {x["speedup"]:.6f} | {x["copy_bytes"]:,} | {x["copy_bytes"]-x["old_copy_bytes"]:+,} | {x["spill_bytes"]:,} | {x["spill_bytes"]-x["old_spill_bytes"]:+,} | {x["old_generation_seconds"]:.3f} → {x["generation_seconds"]:.3f} |')
    lines += ["", "完整來源雜湊、搬運分類、逐階段耗時、所有exact候選診斷及退步記錄見同目錄 RESULTS_V10_1.json。"]
    path.write_text("\n".join(lines)+"\n",encoding="utf-8")


def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run",action="append",help="variant=directory override")
    p.add_argument("--out",type=Path,default=root/"reports/upgrade/RESULTS_V10_1.json")
    p.add_argument("--legacy-smoke",type=Path,default=root/"runs/upgrade/official_exact_legacy/summary.json")
    args=p.parse_args();names=("exact_pool","exact")
    dirs={name:root/"runs/upgrade"/("official_"+name) for name in names}
    for value in args.run or []:
        name,path=value.split("=",1);assert name in names;dirs[name]=Path(path).resolve()
    preservation_path=root/"reports/upgrade/v10_0_baseline_manifest.json";preservation=read(preservation_path)
    protected={(root/name).resolve() for name in preservation["evidence_sha256"]}
    assert args.out.resolve() not in protected,"Do not overwrite preserved v10.0 evidence"
    for name,digest in preservation["evidence_sha256"].items():assert sha(root/name)==digest,("v10.0 evidence changed",name)
    for value in preservation["binaries"].values():assert sha(root/value["saved"])==value["sha256"]
    old_path=root/preservation["baseline_run"];old=read(old_path)
    assert old["completed"] and len(old["rows"])==40 and old["source_sha256"]==preservation["source_sha256"]
    previous={(x["case"],x["cores"]):x for x in old["rows"]}
    expected={(f"case_{i:02}",n) for i in range(10) for n in (2,3,4,5)};assert set(previous)==expected
    old_plans={key:validate_row(root,row) for key,row in previous.items()}
    all_rows=[];summaries={};metadata={};sources=None
    for name in names:
        run_path=dirs[name]/"summary.json";run=read(run_path)
        assert run["completed"] and run["version"]=="0.10.1" and run["variant"]==name and len(run["rows"])==40
        assert {(x["case"],x["cores"]) for x in run["rows"]}==expected
        assert run["baseline_manifest_sha256"]==sha(preservation_path) and run["baseline_summary_sha256"]==sha(old_path)
        if sources is None:sources=run["source_sha256"]
        assert run["source_sha256"]==sources,"variant sources differ"
        for file,digest in sources.items():assert sha(root/file)==digest,("source changed",file)
        assert sha(Path(run["solver_path"]))==run["solver_executable_sha256"]
        assert run["config_sha256"]==old["config_sha256"] and run["official_source_sha256"]==old["official_source_sha256"]
        assert sha(root/"references/hackathon_20261030/data/config.txt")==run["config_sha256"]
        for file,digest in run["official_source_sha256"].items():assert sha(root/"references/hackathon_20261030"/file)==digest
        rows=[]
        for current in sorted(run["rows"],key=lambda x:(x["cores"],x["case"])):
            key=current["case"],current["cores"];before=previous[key];plan=validate_row(root,current)
            assert current["singlecore_cycles"]==before["singlecore_cycles"] and current["input_sha256"]==before["input_sha256"]
            s=current["generation"]["v10_stats"];assert s["stage"]==name
            assert s["legacy_exact_cycles"]==before["cycles"] and s["after_exact_cycles"]==current["cycles"]
            assert current["cycles"]<=s["pool_exact_cycles"]<=s["legacy_exact_cycles"]
            assert s["after_total_bytes"]==current["data_movement_bytes"]["scheduled_copy_bytes"]
            assert s["exact_baseline_valid"] and s["exact_evaluations"]<=(6 if name=="exact_pool" else 14)
            if name=="exact_pool":assert current["cycles"]==s["pool_exact_cycles"]
            row=dict(variant=name,case=key[0],cores=key[1],singlecore_cycles=current["singlecore_cycles"],input_sha256=current["input_sha256"],
                     plan_changed=current["plan_sha256"]!=before["plan_sha256"],mapping_changed=mapping(plan)!=mapping(old_plans[key]),
                     diagnostics={k:v for k,v in current["generation"].items() if k.endswith("_stats")})
            for prefix,value in (("",current),("old_",before)):
                for field in ("cycles","speedup","plan","result","plan_sha256","result_sha256","data_movement_bytes"):row[prefix+field]=value[field]
                for field,mkey in (("copy_bytes","scheduled_copy_bytes"),("spill_bytes","spill_added_copy_bytes"),("partition_bytes","partition_added_copy_bytes")):
                    row[prefix+field]=value["data_movement_bytes"][mkey]
                row[prefix+"generation_seconds"]=value["generation"]["seconds"]
            rows.append(row)
        all_rows.extend(rows);summaries[name]=dict(summary=summarize(rows),by_cores={str(n):summarize([x for x in rows if x["cores"]==n]) for n in (2,3,4,5)})
        metadata[name]=dict(run=run_path.relative_to(root).as_posix(),run_sha256=sha(run_path),solver_path=run["solver_path"],solver_executable_sha256=run["solver_executable_sha256"],solver_extra=run["solver_extra"])
    pool={(x["case"],x["cores"]):x for x in all_rows if x["variant"]=="exact_pool"}
    full=[x for x in all_rows if x["variant"]=="exact"]
    for row in full:
        assert row["diagnostics"]["v10_stats"]["pool_exact_cycles"]==pool[row["case"],row["cores"]]["cycles"],("pool stage differs across independent variants",row["case"],row["cores"])
    tail_origin={phase:dict(selected_points=0,cycle_improved_points=0,traffic_only_points=0,gain_cycles=0,rows=[]) for phase in ("timeline","spill_exact")}
    for row in full:
        stats=row["diagnostics"]["v10_stats"];before=pool[row["case"],row["cores"]]
        improved=[x for x in stats["exact_candidates"] if x["phase"] in tail_origin and x["improved"]]
        row["tail_source"]=None
        if improved:
            winner=improved[-1];phase=winner["phase"]
            assert winner["cycles"]==row["cycles"] and winner["route_bytes"]+winner["spill_bytes"]==row["copy_bytes"]
            assert winner["label"]==stats["selected_label"]
            row["tail_source"]=phase;gain=before["cycles"]-row["cycles"]
            entry=tail_origin[phase];entry["selected_points"]+=1;entry["gain_cycles"]+=gain
            entry["cycle_improved_points"]+=gain>0;entry["traffic_only_points"]+=gain==0
            entry["rows"].append(dict(case=row["case"],cores=row["cores"],label=winner["label"],pool_cycles=before["cycles"],cycles=row["cycles"],gain_cycles=gain,
                                      copy_bytes_change=row["copy_bytes"]-before["copy_bytes"],spill_bytes_change=row["spill_bytes"]-before["spill_bytes"]))
        else:
            assert row["cycles"]==before["cycles"] and row["copy_bytes"]==before["copy_bytes"],("tail changed output without improved audit",row["case"],row["cores"])
    pairs=[(x["cycles"],pool[x["case"],x["cores"]]["cycles"]) for x in full]
    incremental=dict(previous_variant="exact_pool",cycles_improved=sum(a<b for a,b in pairs),cycles_equal=sum(a==b for a,b in pairs),cycles_regressed=sum(a>b for a,b in pairs),
                     **{field+"_change_percent":change(summaries["exact"]["summary"][field],summaries["exact_pool"]["summary"][field]) for field in ("mean_speedup","cycles","copy_bytes","spill_bytes","generation_mean_seconds")})
    smoke=None
    if args.legacy_smoke.exists():
        item=read(args.legacy_smoke);assert item["variant"]=="exact_legacy" and item["source_sha256"]==sources
        assert sha(Path(item["solver_path"]))==item["solver_executable_sha256"]
        assert item["rows"]
        requested={(name,int(n)) for name,n in (x.rsplit(":",1) for x in item["requested_points"])} if item["requested_points"] else {
            (name,n) for name in (item["requested_cases"] or [f"case_{i:02}" for i in range(10)]) for n in item["core_limits"]}
        assert {(x["case"],x["cores"]) for x in item["rows"]}==requested,"legacy smoke incomplete"
        for x in item["rows"]:
            assert x["status"] in ("generated","ok")
            b=previous[x["case"],x["cores"]];assert x["plan_sha256"]==b["plan_sha256"]==sha(root/x["plan"])
            assert x["generation"]["v10_stats"]["legacy_exact_cycles"]==b["cycles"]
        smoke=dict(plans_identical=len(item["rows"]),official_scored=sum(x["status"]=="ok" for x in item["rows"]),summary=str(args.legacy_smoke),sha256=sha(args.legacy_smoke))
    result=dict(version="0.10.1",comparison_version="0.10.0",completed=True,source_sha256=sources,baseline_manifest_sha256=sha(preservation_path),baseline_summary_sha256=sha(old_path),
                variants=summaries,incremental_comparison=incremental,tail_origin=tail_origin,legacy_smoke=smoke,run_metadata=metadata,rows=all_rows,
                notes=["Each new variant has 40 independently generated and officially scored points; no result was filtered.",
                       "legacy_exact_cycles is checked against preserved v10.0 official cycles for every point.",
                       "tail_seconds is included within exact_seconds, not additional to it.",
                       "The historical baseline time may include environment differences; phase times are separately recorded.",
                       "Tail attribution uses the evaluated winner's phase; another generator may propose the same stream and be deduplicated."])
    args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(result,indent=2),encoding="utf-8")
    markdown(args.out.with_name(args.out.stem+"_zh.md"),all_rows,summaries,incremental,tail_origin)
    markdown(args.out.with_name(args.out.stem+"_FINAL_zh.md"),all_rows,summaries,incremental,tail_origin,True)
    print(json.dumps(dict(variants=summaries,incremental_comparison=incremental,tail_origin=tail_origin,legacy_smoke=smoke),indent=2))

if __name__=="__main__":main()
