"""Compare independent v11 greedy-only / annealed40 with tested v10.1 exact40."""
import argparse
from collections import Counter
import json
from pathlib import Path
import statistics
from run_v11_official import read,sha,verify_baseline,check_generation


def pct(a,b):return 100*(a/b-1) if b else None

def mapping(plan):return {v:c for c,stream in enumerate(plan["core_schedules"]) for v in stream}

def verify_row(root,row):
    assert row["status"]=="ok"
    for field in ("plan","result"):assert sha(root/row[field])==row[field+"_sha256"]
    assert sha(root/"references/hackathon_20261030/data"/(row["case"]+".json"))==row["input_sha256"]
    result=read(root/row["result"]);assert result["makespan"]==row["cycles"] and result["num_cores"]==row["cores"]
    assert result["data_movement_bytes"]==row["data_movement_bytes"]
    m=row["data_movement_bytes"]
    assert m["scheduled_copy_bytes"]==sum(m[k] for k in ("original_graph_copy_bytes","partition_added_copy_bytes","spill_added_copy_bytes"))
    assert row["speedup"]==row["singlecore_cycles"]/row["cycles"]
    return read(root/row["plan"])


def statistics_for(rows):
    z=dict(count=len(rows),mean_speedup=statistics.mean(x["speedup"] for x in rows),old_mean_speedup=statistics.mean(x["old_speedup"] for x in rows),
           generation_mean_seconds=statistics.mean(x["generation_seconds"] for x in rows),old_generation_mean_seconds=statistics.mean(x["old_generation_seconds"] for x in rows),
           generation_min_seconds=min(x["generation_seconds"] for x in rows),generation_max_seconds=max(x["generation_seconds"] for x in rows),
           changed_plans=sum(x["plan_changed"] for x in rows),changed_assignments=sum(x["assignment_changed"] for x in rows))
    z["mean_speedup_change_percent"]=pct(z["mean_speedup"],z["old_mean_speedup"])
    for name in ("cycles","copy_bytes","spill_bytes","partition_bytes"):
        z[name]=sum(x[name] for x in rows);z["old_"+name]=sum(x["old_"+name] for x in rows)
        z[name+"_change_percent"]=pct(z[name],z["old_"+name])
        z[name+"_improved"]=sum(x[name]<x["old_"+name] for x in rows)
        z[name+"_equal"]=sum(x[name]==x["old_"+name] for x in rows)
        z[name+"_regressed"]=sum(x[name]>x["old_"+name] for x in rows)
    stats=[x["diagnostics"]["annealing_stats"] for x in rows]
    z["stage_mean_seconds"]={name:statistics.mean(s[name] for s in stats) for name in (
        "baseline_seconds","candidate_seconds","annealing_seconds","greedy_exact_seconds","sa_exact_seconds","total_seconds")}
    z["work_totals"]={name:sum(s[name] for s in stats) for name in (
        "greedy_generated","greedy_unique","sa_generated","sa_unique","proxy_evaluations","exact_evaluations","exact_invalid")}
    z["selected_phase_counts"]=dict(Counter(x["winner_phase"] for x in rows))
    z["exact_rejections"]=[dict(case=x["case"],cores=x["cores"],candidates=[c for c in x["diagnostics"]["annealing_stats"]["exact_candidates"] if c.get("error")])
                           for x in rows if x["diagnostics"]["annealing_stats"]["exact_invalid"]]
    counters=Counter();runs=0
    def walk(obj):
        nonlocal runs
        if isinstance(obj,list):
            for child in obj:walk(child)
        elif isinstance(obj,dict):
            if all(k in obj for k in ("proposals","evaluated","initial_energy")):
                runs+=1
                for k in ("proposals","evaluated","move_proposals","swap_proposals","accepted","uphill_accepted","rejected","skipped_size","skipped_budget","skipped_noop","touched_nodes","undo_nodes","rebasings"):
                    if k in obj:counters[k]+=obj[k]
            else:
                for child in obj.values():walk(child)
    for s in stats:walk(s.get("runs",[]))
    z["sa_runs_with_counters"]=runs;z["sa_counter_totals"]=dict(counters)
    return z


def write_markdown(path,rows,variants,incremental,final_only=False):
    names=("annealed",) if final_only else ("greedy_only","annealed")
    lines=["# v11 正式版40組官方結果" if final_only else "# v11 greedy→SA 的80組官方比較","",
           "比較基準為保留的 v10.1 exact40。單核加速比均以同一官方單核cycles除以當前cycles計算；搬運單位為bytes。每個新方案獨立生成並測量完整程序秒數。","",
           "| 方案 | 平均加速比 | 相對v10.1 | cycles總變化 | COPY總變化 | 溢出總變化 | 平均生成秒 |","|---|---:|---:|---:|---:|---:|---:|"]
    z=variants["annealed"]["summary"];lines.append(f'| v10.1 | {z["old_mean_speedup"]:.6f}× | — | — | — | — | {z["old_generation_mean_seconds"]:.3f} |')
    for name in names:
        z=variants[name]["summary"];lines.append(f'| {name} | {z["mean_speedup"]:.6f}× | {z["mean_speedup_change_percent"]:+.4f}% | {z["cycles_change_percent"]:+.4f}% | {z["copy_bytes_change_percent"]:+.4f}% | {z["spill_bytes_change_percent"]:+.4f}% | {z["generation_mean_seconds"]:.3f} |')
    z=incremental
    lines += ["",f'退火相對greedy-only：平均加速比 {z["mean_speedup_change_percent"]:+.4f}%；cycles改善{z["cycles_improved"]}、相同{z["cycles_equal"]}、退步{z["cycles_regressed"]}組。',"",
              "baseline_seconds僅是新增v11階段的基準精算，不是完整v10.1前綴時間；annealing_seconds包含於candidate_seconds。total_seconds是整個新增v11階段；不能與它的子項重複相加。歷史v10.1耗時可能包含環境差異。"]
    for name in names:
        for n in (2,3,4,5):
            z=variants[name]["by_cores"][str(n)]
            lines += ["",f"## {name}：{n}核","",f'平均加速比 {z["old_mean_speedup"]:.6f}× → {z["mean_speedup"]:.6f}×（{z["mean_speedup_change_percent"]:+.4f}%）。',"",
                      "| 用例 | cycles（v10.1→v11） | 單核加速比（舊→新） | 新總COPY | ΔCOPY | 新溢出COPY | Δ溢出 | 最終來源 | 生成秒（舊→新） |",
                      "|---|---:|---:|---:|---:|---:|---:|---|---:|"]
            for x in rows:
                if x["variant"]!=name or x["cores"]!=n:continue
                lines.append(f'| {x["case"]} | {x["old_cycles"]:,} → {x["cycles"]:,} | {x["old_speedup"]:.6f} → {x["speedup"]:.6f} | {x["copy_bytes"]:,} | {x["copy_bytes"]-x["old_copy_bytes"]:+,} | {x["spill_bytes"]:,} | {x["spill_bytes"]-x["old_spill_bytes"]:+,} | {x["winner_phase"]} | {x["old_generation_seconds"]:.3f} → {x["generation_seconds"]:.3f} |')
    lines += ["","SA接受或降低的是代理能量；不能等同於官方cycles改善。最終候選由官方等價精算選擇，完整候選審計、SA計數、搬運分類與來源雜湊見RESULTS_V11.json。"]
    path.write_text("\n".join(lines)+"\n",encoding="utf-8")


def main():
    root=Path(__file__).resolve().parents[1];p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run",action="append",help="variant=directory override")
    p.add_argument("--out",type=Path,default=root/"reports/RESULTS_V11.json")
    p.add_argument("--baseline-smoke",type=Path,default=root/"runs/official_baseline/summary.json")
    args=p.parse_args();names=("greedy_only","annealed");dirs={name:root/"runs"/("official_"+name) for name in names}
    for value in args.run or []:
        name,path=value.split("=",1);assert name in names;dirs[name]=Path(path).resolve()
    preservation,old,old_root=verify_baseline(root);manifest_path=root/"reports/v10_1_baseline_manifest.json"
    assert old_root!=args.out.resolve() and old_root not in args.out.resolve().parents,"Do not overwrite baseline"
    previous={(x["case"],x["cores"]):x for x in old["rows"]};expected={(f"case_{i:02}",n) for i in range(10) for n in (2,3,4,5)}
    assert set(previous)==expected
    old_plans={key:verify_row(old_root,x) for key,x in previous.items()}
    all_rows=[];summaries={};metadata={};source=None
    for name in names:
        path=dirs[name]/"summary.json";run=read(path)
        assert run["completed"] and run["version"]=="0.11.0" and run["variant"]==name and len(run["rows"])==40
        assert {(x["case"],x["cores"]) for x in run["rows"]}==expected
        assert run["baseline_manifest_sha256"]==sha(manifest_path) and run["baseline_summary_sha256"]==sha(old_root/preservation["baseline_run"])
        if source is None:source=run["source_sha256"]
        assert source==run["source_sha256"],"variant sources differ"
        for file,digest in source.items():assert sha(root/file)==digest,("source changed",file)
        assert sha(Path(run["solver_path"]))==run["solver_executable_sha256"]
        assert run["config_sha256"]==old["config_sha256"]==sha(root/"references/hackathon_20261030/data/config.txt")
        assert run["official_source_sha256"]==old["official_source_sha256"]
        for file,digest in run["official_source_sha256"].items():assert sha(root/"references/hackathon_20261030"/file)==digest
        rows=[]
        for current in sorted(run["rows"],key=lambda x:(x["cores"],x["case"])):
            key=current["case"],current["cores"];before=previous[key];plan=verify_row(root,current)
            check_generation(current["generation"],name,before,current["plan_sha256"])
            assert current["singlecore_cycles"]==before["singlecore_cycles"] and current["input_sha256"]==before["input_sha256"]
            stats=current["generation"]["annealing_stats"]
            assert current["cycles"]==stats["after_cycles"] and current["data_movement_bytes"]["scheduled_copy_bytes"]==stats["after_bytes"]
            assignment_changed=mapping(plan)!=mapping(old_plans[key]);assert assignment_changed==stats["assignment_changed"]
            changed=current["plan_sha256"]!=before["plan_sha256"];assert changed==stats["selected_new"]
            improved=[x for x in stats["exact_candidates"] if x["improved"]]
            winner=improved[-1] if improved else stats["exact_candidates"][0]
            assert winner["cycles"]==current["cycles"] and winner["route_bytes"]+winner["spill_bytes"]==stats["after_bytes"]
            assert winner["label"]==stats["selected_label"]
            row=dict(variant=name,case=key[0],cores=key[1],singlecore_cycles=current["singlecore_cycles"],input_sha256=current["input_sha256"],
                     plan_changed=changed,assignment_changed=assignment_changed,winner_phase=winner["phase"],
                     diagnostics={k:v for k,v in current["generation"].items() if k.endswith("_stats")})
            for prefix,value in (("",current),("old_",before)):
                for field in ("cycles","speedup","plan_sha256","result_sha256","data_movement_bytes"):row[prefix+field]=value[field]
                for field in ("plan","result"):row[prefix+field]=("../dag_chain_solver_v10/" if prefix else "")+value[field]
                for field,mkey in (("copy_bytes","scheduled_copy_bytes"),("spill_bytes","spill_added_copy_bytes"),("partition_bytes","partition_added_copy_bytes")):
                    row[prefix+field]=value["data_movement_bytes"][mkey]
                row[prefix+"generation_seconds"]=value["generation"]["seconds"]
            rows.append(row)
        all_rows.extend(rows);summaries[name]=dict(summary=statistics_for(rows),by_cores={str(n):statistics_for([x for x in rows if x["cores"]==n]) for n in (2,3,4,5)})
        metadata[name]=dict(run=str(path),run_sha256=sha(path),solver_path=run["solver_path"],solver_executable_sha256=run["solver_executable_sha256"],solver_extra=run["solver_extra"])
    greedy={(x["case"],x["cores"]):x for x in all_rows if x["variant"]=="greedy_only"};annealed=[x for x in all_rows if x["variant"]=="annealed"]
    for row in annealed:
        before=greedy[row["case"],row["cores"]];s=row["diagnostics"]["annealing_stats"]
        assert (s["greedy_cycles"],s["greedy_bytes"])==(before["cycles"],before["copy_bytes"]),("greedy prefix differs across variants",row["case"],row["cores"])
        assert (row["cycles"],row["copy_bytes"])<=(before["cycles"],before["copy_bytes"])
    pairs=[(x["cycles"],greedy[x["case"],x["cores"]]["cycles"]) for x in annealed]
    incremental=dict(previous_variant="greedy_only",cycles_improved=sum(a<b for a,b in pairs),cycles_equal=sum(a==b for a,b in pairs),cycles_regressed=sum(a>b for a,b in pairs),
                     **{k+"_change_percent":pct(summaries["annealed"]["summary"][k],summaries["greedy_only"]["summary"][k]) for k in ("mean_speedup","cycles","copy_bytes","spill_bytes","generation_mean_seconds")})
    smoke=None
    if args.baseline_smoke.exists():
        item=read(args.baseline_smoke);assert item["variant"]=="baseline" and item["source_sha256"]==source
        assert item["completed"] and item["version"]=="0.11.0"
        assert {(x["case"],x["cores"]) for x in item["rows"]}=={("case_00",2),("case_00",5),("case_08",4),("case_09",5)} and len(item["rows"])==4
        assert sha(Path(item["solver_path"]))==item["solver_executable_sha256"] and item["rows"]
        for x in item["rows"]:
            verify_row(root,x);before=previous[x["case"],x["cores"]]
            assert x["plan_sha256"]==sha(root/x["plan"]);check_generation(x["generation"],"baseline",before,x["plan_sha256"])
        smoke=dict(plans_identical=len(item["rows"]),official_scored=sum(x["status"]=="ok" for x in item["rows"]),summary=str(args.baseline_smoke),sha256=sha(args.baseline_smoke))
    result=dict(version="0.11.0",comparison_version="0.10.1",completed=True,source_sha256=source,baseline_manifest_sha256=sha(manifest_path),
                baseline_source_note="Tested ZIP bytes are authoritative; 21 current v10 source files have recorded LF/CRLF-only differences.",
                variants=summaries,incremental_comparison=incremental,baseline_smoke=smoke,run_metadata=metadata,rows=all_rows,
                notes=["Independent full-process generation timing; no points filtered.","SA energies and acceptance counters are proxies, not official score improvements.",
                       "baseline_seconds measures one extra exact baseline score within v11, not the full v10.1 prefix.",
                       "annealing_seconds is included in candidate_seconds; total_seconds includes the whole added v11 stage.",
                       "Historical baseline generation times may include environment differences."])
    args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(result,indent=2),encoding="utf-8")
    write_markdown(args.out.with_name(args.out.stem+"_zh.md"),all_rows,summaries,incremental)
    write_markdown(args.out.with_name(args.out.stem+"_FINAL_zh.md"),all_rows,summaries,incremental,True)
    print(json.dumps(dict(variants=summaries,incremental_comparison=incremental,baseline_smoke=smoke),indent=2))

if __name__=="__main__":main()
