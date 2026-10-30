"""Report all 40 deadline-enabled results, including regressions and time overruns."""
import argparse
from collections import Counter
import json
from pathlib import Path
import statistics
from run_v11_budget import read,sha,verify_baseline,check_generation
from summarize_v11 import mapping,verify_row


def pct(a,b):return 100*(a/b-1) if b else None


def aggregate(rows):
    result=dict(count=len(rows),mean_speedup=statistics.mean(x['speedup'] for x in rows),
                old_mean_speedup=statistics.mean(x['old_speedup'] for x in rows),
                generation_mean_seconds=statistics.mean(x['generation_seconds'] for x in rows),
                old_generation_mean_seconds=statistics.mean(x['old_generation_seconds'] for x in rows),
                generation_min_seconds=min(x['generation_seconds'] for x in rows),
                generation_max_seconds=max(x['generation_seconds'] for x in rows),
                under_15_seconds=sum(x['generation_seconds']<15 for x in rows),
                between_15_and_20_seconds=sum(15<=x['generation_seconds']<=20 for x in rows),
                over_20_seconds=sum(x['generation_seconds']>20 for x in rows),
                met_20_second_target=all(x['generation_seconds']<=20 for x in rows),
                changed_plans=sum(x['plan_changed'] for x in rows),
                changed_assignments=sum(x['assignment_changed_vs_v10_1'] for x in rows),
                winner_phase_counts=dict(Counter(x['winner_phase'] for x in rows)))
    for field in ('cycles','copy_bytes','spill_bytes','partition_bytes'):
        result[field]=sum(x[field] for x in rows);result['old_'+field]=sum(x['old_'+field] for x in rows)
        result[field+'_change_percent']=pct(result[field],result['old_'+field])
        for label,op in (('improved',lambda a,b:a<b),('equal',lambda a,b:a==b),('regressed',lambda a,b:a>b)):
            result[field+'_'+label]=sum(op(x[field],x['old_'+field]) for x in rows)
    result['mean_speedup_change_percent']=pct(result['mean_speedup'],result['old_mean_speedup'])
    stats=[x['diagnostics']['annealing_stats'] for x in rows]
    result['annealing_work_totals']={name:sum(s[name] for s in stats) for name in (
        'greedy_generated','greedy_unique','sa_generated','sa_unique','proxy_evaluations','exact_evaluations','exact_invalid')}
    result['annealing_stage_mean_seconds']={name:statistics.mean(s[name] for s in stats) for name in (
        'baseline_seconds','candidate_seconds','annealing_seconds','greedy_exact_seconds','sa_exact_seconds','total_seconds')}
    checkpoints={phase:dict(mean_speedup=statistics.mean(x['singlecore_cycles']/x['diagnostics']['annealing_stats'][phase+'_cycles'] for x in rows),
                            cycles=sum(s[phase+'_cycles'] for s in stats),copy_bytes=sum(s[phase+'_bytes'] for s in stats))
                 for phase in ('baseline','greedy','after')}
    for start,end in (('baseline','greedy'),('greedy','after')):
        checkpoints[start+'_to_'+end]=dict(
            cycles_improved=sum(s[end+'_cycles']<s[start+'_cycles'] for s in stats),
            cycles_equal=sum(s[end+'_cycles']==s[start+'_cycles'] for s in stats),
            cycles_regressed=sum(s[end+'_cycles']>s[start+'_cycles'] for s in stats),
            cycles_or_bytes_improved=sum((s[end+'_cycles'],s[end+'_bytes'])<(s[start+'_cycles'],s[start+'_bytes']) for s in stats),
            mean_speedup_change_percent=pct(checkpoints[end]['mean_speedup'],checkpoints[start]['mean_speedup']))
    result['same_run_internal_checkpoints']=checkpoints
    result['phase_mean_seconds']={name:statistics.mean(next(p['seconds'] for p in x['diagnostics']['budget_stats']['phases'] if p['name']==name) for x in rows)
                                  for name in ('seed','feedback','communication','v10','annealing')}
    result['phase_deadline_reached_counts']={name:sum(next(p['deadline_reached'] for p in x['diagnostics']['budget_stats']['phases'] if p['name']==name) for x in rows)
                                            for name in ('seed','feedback','communication','v10','annealing')}
    result['exact_rejections']=[dict(case=x['case'],cores=x['cores'],candidates=[c for c in x['diagnostics']['annealing_stats']['exact_candidates'] if c.get('error')])
                                for x in rows if x['diagnostics']['annealing_stats']['exact_invalid']]
    return result


def markdown(path,result):
    z=result['summary'];rows=result['rows']
    lines=['# v11.1 整體20秒版：40組官方結果','',
           '比較基準為保存的 v10.1 官方40點。加速比＝同一官方單核cycles／當前cycles；搬運以bytes表示。全部退步與超時點均保留。','',
           '| 核數 | 平均加速比（v10.1→新版） | 相對變化 | cycles改善／相同／退步 | 平均生成秒 | 最慢秒 | 超20秒 |',
           '|---|---:|---:|---:|---:|---:|---:|']
    for n in ('2','3','4','5','全部'):
        s=z if n=='全部' else result['by_cores'][n]
        lines.append(f'| {n} | {s["old_mean_speedup"]:.6f}→{s["mean_speedup"]:.6f} | {s["mean_speedup_change_percent"]:+.4f}% | {s["cycles_improved"]}/{s["cycles_equal"]}/{s["cycles_regressed"]} | {s["generation_mean_seconds"]:.3f} | {s["generation_max_seconds"]:.3f} | {s["over_20_seconds"]} |')
    lines+=['',f'本次實測20秒目標：{"40/40達標" if z["met_20_second_target"] else str(z["over_20_seconds"])+"點超出，不能宣稱全部達標"}。採合作式截止；此結果不保證所有輸入及機器都在20秒內。簡單點提早完成，不刻意等待到15秒。',
            '', '完整生成秒數由外部程序計時，包含啟動與輸出；budget_stats.elapsed_seconds從讀入前至保存完成，少了程序啟動。各phase秒數與其絕對截止點分開記錄，不能將截止點相加。歷史生成耗時受機器狀態影響。']
    c=z['same_run_internal_checkpoints'];t=z['annealing_stage_mean_seconds']
    lines+=['',f'同一次生成的內部精算checkpoint平均加速比：限時前段 {c["baseline"]["mean_speedup"]:.6f}× → greedy {c["greedy"]["mean_speedup"]:.6f}× → SA最終 {c["after"]["mean_speedup"]:.6f}×。greedy改善cycles {c["baseline_to_greedy"]["cycles_improved"]}點；SA再改善 {c["greedy_to_after"]["cycles_improved"]}點。這是相同run的內部checkpoint比較，並非各階段獨立跑官方評分的消融實驗。',
            '',f'SA迭代本體平均 {t["annealing_seconds"]:.3f}秒；整個新增v11階段平均 {t["total_seconds"]:.3f}秒。SA本體含於候選生成時間，整個階段另含基準／greedy／SA候選精算等，不能重複相加。']
    for n in (2,3,4,5):
        lines+=['',f'## {n}核','',
                '| 用例 | cycles（舊→新） | 加速比（舊→新） | 總COPY（舊→新） | 溢出COPY（舊→新） | 生成秒（舊→新） | 最終v11來源 |',
                '|---|---:|---:|---:|---:|---:|---|']
        for x in rows:
            if x['cores']!=n:continue
            lines.append(f'| {x["case"]} | {x["old_cycles"]:,}→{x["cycles"]:,} | {x["old_speedup"]:.6f}→{x["speedup"]:.6f} | {x["old_copy_bytes"]:,}→{x["copy_bytes"]:,} | {x["old_spill_bytes"]:,}→{x["spill_bytes"]:,} | {x["old_generation_seconds"]:.3f}→{x["generation_seconds"]:.3f} | {x["winner_phase"]} |')
    lines+=['','最終v11來源中的baseline指限時前段產生的方案，不代表完整v10.1。限時截斷可能造成相對v10.1退步。SA代理能量下降不等於官方cycles改善。JSON保留每個候選、預算階段時間、搬運分類及雜湊。']
    path.write_text('\n'.join(lines)+'\n',encoding='utf-8')


def main():
    root=Path(__file__).resolve().parents[1];p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',type=Path,default=root/'runs/budget20_official')
    p.add_argument('--out',type=Path,default=root/'reports/budget20/RESULTS_V11_BUDGET20.json')
    args=p.parse_args();run_path=args.run/'summary.json';run=read(run_path)
    preservation,old,old_root=verify_baseline(root)
    assert old_root not in args.out.resolve().parents
    assert run['completed'] and run['version']=='0.11.1' and run['variant']=='budget20_annealed'
    expected={(f'case_{i:02}',n) for i in range(10) for n in (2,3,4,5)}
    assert len(run['rows'])==40 and {(x['case'],x['cores']) for x in run['rows']}==expected
    before={(x['case'],x['cores']):x for x in old['rows']};assert set(before)==expected
    manifest_path=root/'reports/v10_1_baseline_manifest.json'
    assert run['baseline_manifest_sha256']==sha(manifest_path)
    assert run['baseline_summary_sha256']==sha(old_root/preservation['baseline_run'])
    for name,digest in run['source_sha256'].items():assert sha(root/name)==digest
    assert sha(Path(run['solver_path']))==run['solver_executable_sha256']
    assert run['config_sha256']==old['config_sha256']==sha(root/'references/hackathon_20261030/data/config.txt')
    assert run['official_source_sha256']==old['official_source_sha256']
    for name,digest in run['official_source_sha256'].items():assert sha(root/'references/hackathon_20261030'/name)==digest
    pilot_path=root/'reports/budget20/unlimited_preservation_manifest.json';pilot=read(pilot_path)
    for name,digest in pilot['files'].items():assert sha(root/name)==digest,('unlimited evidence changed',name)
    rows=[]
    for current in sorted(run['rows'],key=lambda x:(x['cores'],x['case'])):
        key=current['case'],current['cores'];prior=before[key]
        plan=verify_row(root,current);old_plan=verify_row(old_root,prior)
        assert current['input_sha256']==prior['input_sha256'] and current['singlecore_cycles']==prior['singlecore_cycles']
        check_generation(current['generation']);stats=current['generation']['annealing_stats']
        assert stats['after_cycles']==current['cycles'] and stats['after_bytes']==current['data_movement_bytes']['scheduled_copy_bytes']
        improved=[x for x in stats['exact_candidates'] if x['improved']]
        winner=improved[-1] if improved else stats['exact_candidates'][0]
        assert winner['cycles']==current['cycles'] and winner['route_bytes']+winner['spill_bytes']==stats['after_bytes']
        assert winner['label']==stats['selected_label']
        row=dict(case=key[0],cores=key[1],singlecore_cycles=current['singlecore_cycles'],input_sha256=current['input_sha256'],
                 plan_changed=current['plan_sha256']!=prior['plan_sha256'],assignment_changed_vs_v10_1=mapping(plan)!=mapping(old_plan),
                 winner_phase=winner['phase'],winner_label=winner['label'],
                 diagnostics={k:v for k,v in current['generation'].items() if k.endswith('_stats')})
        for prefix,value in (('',current),('old_',prior)):
            for field in ('cycles','speedup','plan_sha256','result_sha256','data_movement_bytes'):row[prefix+field]=value[field]
            for field in ('plan','result'):row[prefix+field]=('../dag_chain_solver_v10/' if prefix else '')+value[field]
            for field,mkey in (('copy_bytes','scheduled_copy_bytes'),('spill_bytes','spill_added_copy_bytes'),('partition_bytes','partition_added_copy_bytes')):
                row[prefix+field]=value['data_movement_bytes'][mkey]
            row[prefix+'generation_seconds']=value['generation']['seconds']
        rows.append(row)
    result=dict(version='0.11.1',completed=True,comparison_version='0.10.1',target_seconds=20,source_sha256=run['source_sha256'],
                baseline_manifest_sha256=sha(manifest_path),unlimited_preservation_manifest_sha256=sha(pilot_path),
                run_metadata=dict(run=str(run_path.resolve()),run_sha256=sha(run_path),solver_path=run['solver_path'],solver_executable_sha256=run['solver_executable_sha256']),
                summary=aggregate(rows),by_cores={str(n):aggregate([x for x in rows if x['cores']==n]) for n in (2,3,4,5)},rows=rows,
                notes=['All regressions and time overruns retained.','Deadline truncation means no nonregression guarantee versus full v10.1.',
                       'baseline in annealing_stats denotes the truncated prefix, not the historical v10.1 result.',
                       'Cooperative cancellation and indivisible work do not provide a universal hard wall-time bound.',
                       'Historical generation wall time may include environmental differences.',
                       'Tested v10.1 ZIP bytes are authoritative; working sources have recorded CRLF/LF-only differences.'])
    args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(result,indent=2),encoding='utf-8')
    markdown(args.out.with_name(args.out.stem+'_zh.md'),result)
    print(json.dumps(dict(summary=result['summary'],by_cores=result['by_cores']),indent=2))


if __name__=='__main__':main()
