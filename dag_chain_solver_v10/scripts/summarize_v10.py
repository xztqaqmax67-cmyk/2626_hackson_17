"""Verify and compare three cumulative v10 experiments with preserved v9."""
import argparse
import hashlib
import json
import statistics
from pathlib import Path


def read(p):
    return json.loads(p.read_text(encoding='utf-8'))


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def pct(a, b):
    return 100 * (a / b - 1) if b else None


def verify_row(root, row):
    assert row['status'] == 'ok'
    assert sha(root/'references/hackathon_20261030/data'/(row['case']+'.json')) == row['input_sha256']
    assert sha(root/row['plan']) == row['plan_sha256']
    assert sha(root/row['result']) == row['result_sha256']
    result = read(root/row['result'])
    assert result['makespan'] == row['cycles'] and result['num_cores'] == row['cores']
    assert result['data_movement_bytes'] == row['data_movement_bytes']
    m = row['data_movement_bytes']
    assert m['scheduled_copy_bytes'] == sum(m[k] for k in ('original_graph_copy_bytes','partition_added_copy_bytes','spill_added_copy_bytes'))
    assert row['speedup'] == row['singlecore_cycles']/row['cycles']
    return read(root/row['plan'])


def statistics_for(rows):
    result = dict(count=len(rows), mean_speedup=statistics.mean(r['speedup'] for r in rows),
                  old_mean_speedup=statistics.mean(r['old_speedup'] for r in rows),
                  generation_mean_seconds=statistics.mean(r['generation_seconds'] for r in rows),
                  generation_min_seconds=min(r['generation_seconds'] for r in rows),
                  generation_max_seconds=max(r['generation_seconds'] for r in rows),
                  old_generation_mean_seconds=statistics.mean(r['old_generation_seconds'] for r in rows),
                  changed_plans=sum(r['plan_changed'] for r in rows),
                  changed_core_assignments=sum(r['mapping_changed'] for r in rows),
                  exact_invalid_candidates=sum(r['diagnostics']['v10_stats']['exact_invalid'] for r in rows),
                  exact_rejection_reasons=[dict(case=r['case'],cores=r['cores'],errors=r['diagnostics']['v10_stats'].get('exact_errors',[]))
                                           for r in rows if r['diagnostics']['v10_stats']['exact_invalid']])
    result['v10_phase_mean_seconds']={name:statistics.mean(r['diagnostics']['v10_stats'][name] for r in rows)
                                      for name in ('candidate_seconds','feedback_seconds','exact_seconds')}
    result['v10_work_totals']={name:sum(r['diagnostics']['v10_stats'][name] for r in rows)
                               for name in ('generated','unique','traced','refined','feedback_starts','feedback_replays','feedback_refinements','exact_evaluations','selected_new')}
    result['mean_speedup_change_percent'] = pct(result['mean_speedup'],result['old_mean_speedup'])
    for key in ('cycles','copy_bytes','spill_bytes','partition_bytes'):
        after,before = sum(r[key] for r in rows),sum(r['old_'+key] for r in rows)
        result[key],result['old_'+key] = after,before
        result[key+'_change_percent'] = pct(after,before)
        for label,compare in [('improved',lambda a,b:a<b),('equal',lambda a,b:a==b),('regressed',lambda a,b:a>b)]:
            result[key+'_'+label] = sum(compare(r[key],r['old_'+key]) for r in rows)
    return result


def write_final_report(path,rows,summary):
    z=summary['summary']
    lines=['# v10 最終版：40 組官方結果（exact）','',
           '此表只列正式 npu_sched 的最終 exact 方案；10個用例×2～5核。所有加速比均使用原官方單核基準，平均值採算術平均。搬運單位為 bytes，生成時間為本機完整程序實測秒數。',
           '',f'平均加速比：v9 {z["old_mean_speedup"]:.6f}× → v10 {z["mean_speedup"]:.6f}×（{z["mean_speedup_change_percent"]:+.4f}%）。',
           f'官方 cycles：改善 {z["cycles_improved"]} 組、相同 {z["cycles_equal"]} 組、退步 {z["cycles_regressed"]} 組。總 COPY {z["copy_bytes_change_percent"]:+.4f}%，溢出 COPY {z["spill_bytes_change_percent"]:+.4f}%。',
           f'平均生成：v9 {z["old_generation_mean_seconds"]:.3f} 秒 → v10 {z["generation_mean_seconds"]:.3f} 秒。v9時間來自先前測量，不能排除環境變動。',
           '', '每列的「Δ」是 v10 減 v9；負數表示搬運減少。最終選擇以官方 cycles 為主，個別點可能用較多搬運換取更短時間。']
    for n in (2,3,4,5):
        z=summary['by_cores'][str(n)]
        lines+=['',f'## {n} 核','',f'平均加速比：{z["old_mean_speedup"]:.6f}× → {z["mean_speedup"]:.6f}×（{z["mean_speedup_change_percent"]:+.4f}%）。',
                '', '| 用例 | cycles（v9→v10） | 單核加速比（v9→v10） | v10總COPY | Δ總COPY | v10溢出COPY | Δ溢出COPY | 生成秒（v9→v10） |',
                '|---|---:|---:|---:|---:|---:|---:|---:|']
        for x in rows:
            if x['cores']!=n:continue
            lines.append(f'| {x["case"]} | {x["old_cycles"]:,} → {x["cycles"]:,} | {x["old_speedup"]:.6f} → {x["speedup"]:.6f} | {x["copy_bytes"]:,} | {x["copy_bytes"]-x["old_copy_bytes"]:+,} | {x["spill_bytes"]:,} | {x["spill_bytes"]-x["old_spill_bytes"]:+,} | {x["old_generation_seconds"]:.3f} → {x["generation_seconds"]:.3f} |')
    lines+=['','完整消融120筆、搬運分類、階段耗時、逐點診斷與來源雜湊見 RESULTS_V10.json；正式原始plan/result/trace/log在 runs/official_exact。']
    path.write_text('\n'.join(lines)+'\n',encoding='utf-8')


def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',action='append',help='variant=directory; default all three runs/official_<variant>')
    p.add_argument('--out',type=Path,default=root/'reports/RESULTS_V10.json')
    args=p.parse_args()
    variants=('tensor_memory','multi_feedback','exact')
    runs={name:root/'runs'/('official_'+name) for name in variants}
    if args.run:
        for value in args.run:
            name,path=value.split('=',1);assert name in variants;runs[name]=Path(path).resolve()
    old_root=root.parent/'dag_chain_solver_v9'
    old_path=old_root/'runs/official_all/summary.json';old=read(old_path)
    assert old['completed'] and len(old['rows'])==40
    preservation=read(root/'reports/v9_preservation_manifest.json')['files']
    for name,digest in preservation.items():assert sha(old_root/name)==digest,('v9 changed',name)
    for name,digest in old['source_sha256'].items():assert sha(old_root/name)==digest,('v9 provenance',name)
    assert sha(Path(old['solver_path']))==old['solver_executable_sha256']
    previous={(r['case'],r['cores']):r for r in old['rows']}
    expected={(f'case_{i:02}',n) for i in range(10) for n in (2,3,4,5)}
    assert set(previous)==expected
    old_plans={key:verify_row(old_root,row) for key,row in previous.items()}
    def mapping(plan):return {v:c for c,stream in enumerate(plan['core_schedules']) for v in stream}
    all_rows=[];summaries={};metadata={};source=None
    for variant in variants:
        path=runs[variant]/'summary.json';run=read(path)
        assert run['completed'] and run['variant']==variant and len(run['rows'])==40
        assert {(r['case'],r['cores']) for r in run['rows']}==expected
        if source is None:source=run['source_sha256']
        assert source==run['source_sha256'],'variant sources differ; use one frozen source revision'
        for name,digest in source.items():assert sha(root/name)==digest,('new source changed',name)
        assert sha(Path(run['solver_path']))==run['solver_executable_sha256']
        assert run['config_sha256']==old['config_sha256']
        assert run['official_source_sha256']==old['official_source_sha256']
        for name,digest in run['official_source_sha256'].items():assert sha(root/'references/hackathon_20261030'/name)==digest
        rows=[]
        for now in sorted(run['rows'],key=lambda r:(r['cores'],r['case'])):
            key=now['case'],now['cores'];before=previous[key];new_plan=verify_row(root,now)
            assert before['input_sha256']==now['input_sha256'] and before['singlecore_cycles']==now['singlecore_cycles']
            stats=now['generation']['v10_stats'];assert stats['stage']==variant
            assert stats['before_total_bytes']==before['data_movement_bytes']['scheduled_copy_bytes'],('v9 entry traffic differs',key)
            assert stats['after_total_bytes']==now['data_movement_bytes']['scheduled_copy_bytes'],('selected traffic differs',variant,key)
            if variant=='exact':
                assert stats['exact_baseline_valid'],('known-valid v9 baseline rejected by timing port',key)
                assert stats['before_exact_cycles']==before['cycles'],('baseline exact cycles differ',key)
                assert stats['after_exact_cycles']==now['cycles'],('selected exact cycles differ',key)
                assert now['cycles']<=before['cycles'],('exact selection regressed versus valid baseline',key)
            else:
                assert stats['after_total_bytes']<=stats['before_total_bytes'],('conservative traffic guard failed',variant,key)
                assert stats['after_estimated_cycles']<=stats['before_estimated_cycles'],('conservative estimate guard failed',variant,key)
            if not stats['selected_new']:
                assert now['plan_sha256']==before['plan_sha256'],('unchanged selection has different plan',variant,key)
            row=dict(variant=variant,case=key[0],cores=key[1],singlecore_cycles=now['singlecore_cycles'],
                     input_sha256=now['input_sha256'],plan_changed=now['plan_sha256']!=before['plan_sha256'],
                     mapping_changed=mapping(new_plan)!=mapping(old_plans[key]),
                     diagnostics={k:v for k,v in now['generation'].items() if k.endswith('_stats')})
            for prefix,value in [('',now),('old_',before)]:
                for field in ('cycles','speedup','plan_sha256','result_sha256'):
                    row[prefix+field]=value[field]
                for field in ('plan','result'):
                    row[prefix+field]=('../dag_chain_solver_v9/' if prefix else '')+value[field]
                movement=value['data_movement_bytes'];row[prefix+'data_movement_bytes']=movement
                for field,mkey in [('copy_bytes','scheduled_copy_bytes'),('spill_bytes','spill_added_copy_bytes'),('partition_bytes','partition_added_copy_bytes')]:
                    row[prefix+field]=movement[mkey]
                row[prefix+'generation_seconds']=value['generation']['seconds']
            rows.append(row)
        all_rows.extend(rows)
        summaries[variant]=dict(summary=statistics_for(rows),by_cores={str(c):statistics_for([r for r in rows if r['cores']==c]) for c in (2,3,4,5)})
        metadata[variant]=dict(run=str(path),run_sha256=sha(path),solver_path=run['solver_path'],
                               solver_executable_sha256=run['solver_executable_sha256'],solver_extra=run['solver_extra'])
    incremental={};previous_variant='v9'
    for variant in variants:
        current=summaries[variant]['summary']
        prior=summaries[previous_variant]['summary'] if previous_variant!='v9' else {
            'mean_speedup':current['old_mean_speedup'],'cycles':current['old_cycles'],'copy_bytes':current['old_copy_bytes'],
            'spill_bytes':current['old_spill_bytes'],'generation_mean_seconds':current['old_generation_mean_seconds']}
        incremental[variant]=dict(previous_variant=previous_variant,**{
            name+'_change_percent':pct(current[name],prior[name])
            for name in ('mean_speedup','cycles','copy_bytes','spill_bytes','generation_mean_seconds')})
        current_rows=[r for r in all_rows if r['variant']==variant]
        previous_rows={(r['case'],r['cores']):r for r in all_rows if r['variant']==previous_variant}
        pairs=[(r['cycles'],r['old_cycles'] if previous_variant=='v9' else previous_rows[r['case'],r['cores']]['cycles']) for r in current_rows]
        incremental[variant]['cycles_improved']=sum(a<b for a,b in pairs)
        incremental[variant]['cycles_equal']=sum(a==b for a,b in pairs)
        incremental[variant]['cycles_regressed']=sum(a>b for a,b in pairs)
        previous_variant=variant
    report=dict(version='0.10.0',comparison_version='0.9.0',completed=True,source_sha256=source,
                variants=summaries,incremental_comparisons=incremental,run_metadata=metadata,old_summary_sha256=sha(old_path),
                v9_preservation_manifest_sha256=sha(root/'reports/v9_preservation_manifest.json'),rows=all_rows,
                notes=['All 120 results use the unchanged official scorer; no regressions are filtered.',
                       'Each variant is generated and timed independently; numbers are full-process wall time, not incremental phase time.',
                       'v9 timing was measured in an earlier run; environment variation is not excluded.',
                       'exact adds exact final selection AND permits total-traffic tradeoffs; this is not a timing-model-only ablation.'])
    args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(report,indent=2),encoding='utf-8')
    lines=['# v10 三個累積方案與 v9 比較','','各階段均獨立生成40組方案並使用未修改的官方評分器。時間是完整程序的實測時間，不是階段增量時間。',
           'exact 階段同時加入精確最後選優及允許總搬運取捨，不能解讀成只換計時模型的消融。','',
           '| 方案 | 平均加速比 | 相對v9 | cycles總變化 | COPY總變化 | 溢出總變化 | 平均生成秒 |','|---|---:|---:|---:|---:|---:|---:|']
    base=summaries['exact']['summary'];lines.append(f'| v9 | {base["old_mean_speedup"]:.6f}× | — | — | — | — | {base["old_generation_mean_seconds"]:.3f} |')
    for name in variants:
        z=summaries[name]['summary'];lines.append(f'| {name} | {z["mean_speedup"]:.6f}× | {z["mean_speedup_change_percent"]:+.4f}% | {z["cycles_change_percent"]:+.4f}% | {z["copy_bytes_change_percent"]:+.4f}% | {z["spill_bytes_change_percent"]:+.4f}% | {z["generation_mean_seconds"]:.3f} |')
    lines+=['','相對前一個累積方案：','','| 方案 | 前一方案 | 平均加速比變化 | cycles變化 | COPY變化 | 平均生成時間變化 |','|---|---|---:|---:|---:|---:|']
    for name in variants:
        z=incremental[name];lines.append(f'| {name} | {z["previous_variant"]} | {z["mean_speedup_change_percent"]:+.4f}% | {z["cycles_change_percent"]:+.4f}% | {z["copy_bytes_change_percent"]:+.4f}% | {z["generation_mean_seconds_change_percent"]:+.4f}% |')
    for variant in variants:
        rejected=summaries[variant]['summary']['exact_invalid_candidates']
        lines+=['',f'## {variant}','',f'精確候選拒絕數：{rejected}；逐點原因保留於JSON的 exact_rejection_reasons。',
                '','| 用例 | 核數 | v9 cycles | 新 cycles | v9／新加速比 | v9／新COPY bytes | v9／新溢出 bytes | 生成秒 |','|---|---:|---:|---:|---:|---:|---:|---:|']
        for x in all_rows:
            if x['variant']!=variant:continue
            lines.append(f'| {x["case"]} | {x["cores"]} | {x["old_cycles"]:,} | {x["cycles"]:,} | {x["old_speedup"]:.6f}/{x["speedup"]:.6f} | {x["old_copy_bytes"]:,}/{x["copy_bytes"]:,} | {x["old_spill_bytes"]:,}/{x["spill_bytes"]:,} | {x["generation_seconds"]:.3f} |')
    lines+=['','分核COPY、診斷、完整雜湊與各核平均值見同名JSON。原始plan/result/trace/log在各runs/official_<variant>目錄。']
    args.out.with_name(args.out.stem+'_zh.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    write_final_report(args.out.with_name(args.out.stem+'_FINAL_zh.md'),[r for r in all_rows if r['variant']=='exact'],summaries['exact'])
    print(json.dumps(summaries,indent=2))

if __name__=='__main__':main()
