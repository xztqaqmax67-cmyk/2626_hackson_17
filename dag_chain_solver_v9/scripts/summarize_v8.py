"""Verify official v8 results and compare to preserved v7, with fixed core assignments."""
import json,hashlib,math,statistics,sys
from pathlib import Path

def read(p): return json.loads(p.read_text(encoding='utf-8'))
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def change(a,b): return 100*(a/b-1) if b else None

def main():
 root=Path(__file__).resolve().parents[1];oldroot=root.parent/'dag_chain_solver_v7'
 measured=read(root/'runs/official_all/summary.json');old=read(oldroot/'reports/RESULTS_2_TO_5_CORES.json')
 assert measured['completed'] and len(measured['rows'])==40 and old['completed']
 for name,h in measured['source_sha256'].items(): assert sha(root/name)==h,name
 assert sha(Path(measured['solver_path']))==measured['solver_executable_sha256']
 assert measured['config_sha256']==old['config']['sha256']
 assert measured['official_source_sha256']==old['official_source_sha256']
 previous={(x['case'],x['cores']):x for x in old['rows']};rows=[]
 for x in measured['rows']:
  assert x['status']=='ok';key=x['case'],x['cores'];p=previous[key]
  assert x['input_sha256']==p['input_sha256']
  assert sha(root/x['plan'])==x['plan_sha256'] and sha(root/x['result'])==x['result_sha256']
  assert sha(oldroot/p['artifacts']['plan'])==p['plan_sha256']
  assert sha(oldroot/p['artifacts']['result'])==p['result_sha256']
  result=read(root/x['result']);assert result['makespan']==x['cycles']
  m=result['data_movement_bytes']; assert m==x['data_movement_bytes']
  assert m['scheduled_copy_bytes']==m['original_graph_copy_bytes']+m['partition_added_copy_bytes']+m['spill_added_copy_bytes']
  newplan=read(root/x['plan']);oldplan=read(oldroot/p['artifacts']['plan'])
  def mapping(plan):return {v:c for c,stream in enumerate(plan['core_schedules']) for v in stream}
  assert mapping(newplan)==mapping(oldplan),(key,'core assignments changed')
  assert newplan['node_to_subgraph']==oldplan['node_to_subgraph']
  assert m['partition_added_copy_bytes']==p['data_movement_bytes']['partition_added_copy_bytes'],key
  assert m['spill_added_copy_bytes']<=p['spill_copy_bytes'],(key,'official spill regression')
  row=dict(case=x['case'],cores=x['cores'],cycles=x['cycles'],speedup=x['speedup'],copy_bytes=m['scheduled_copy_bytes'],spill_bytes=m['spill_added_copy_bytes'],generation_seconds=x['generation']['seconds'],v7_cycles=p['cycles'],v7_speedup=p['speedup'],v7_copy_bytes=p['copy_bytes'],v7_spill_bytes=p['spill_copy_bytes'],v7_generation_seconds=p['generation_seconds'],cycles_change_percent=change(x['cycles'],p['cycles']),spill_change_percent=change(m['spill_added_copy_bytes'],p['spill_copy_bytes']),data_movement_bytes=m,plan=x['plan'],result=x['result'],plan_sha256=x['plan_sha256'],result_sha256=x['result_sha256'],fixed_mapping=True,core_order_changed=newplan['core_schedules']!=oldplan['core_schedules'])
  rows.append(row)
 rows.sort(key=lambda x:(x['cores'],x['case']))
 def summary(data):
  value={'count':len(data),'mean_speedup':statistics.mean(x['speedup'] for x in data),'v7_mean_speedup':statistics.mean(x['v7_speedup'] for x in data),'generation_mean_seconds':statistics.mean(x['generation_seconds'] for x in data),'v7_generation_mean_seconds':statistics.mean(x['v7_generation_seconds'] for x in data),'changed_plans':sum(x['core_order_changed'] for x in data)}
  for name in ['cycles','copy_bytes','spill_bytes']:
   new=sum(x[name] for x in data);before=sum(x['v7_'+name] for x in data)
   value.update({name:new,'v7_'+name:before,name+'_change_percent':change(new,before),name+'_improved':sum(x[name]<x['v7_'+name] for x in data),name+'_equal':sum(x[name]==x['v7_'+name] for x in data),name+'_regressed':sum(x[name]>x['v7_'+name] for x in data)})
  return value
 report=dict(version='0.8.0',completed=True,source_sha256=measured['source_sha256'],solver_executable_sha256=measured['solver_executable_sha256'],official_source_sha256=measured['official_source_sha256'],config_sha256=measured['config_sha256'],measured_summary_sha256=sha(root/'runs/official_all/summary.json'),v7_report_sha256=sha(oldroot/'reports/RESULTS_2_TO_5_CORES.json'),rows=rows,summary=summary(rows),by_cores={str(c):summary([x for x in rows if x['cores']==c]) for c in range(2,6)},notes=['Only within-core order changed; every core assignment was checked against v7.','All final cycles and COPY values are from the unmodified official scorer.','The C++ spill trace ports Step1/task preparation/Step2, not the full Step3 timing model.','Generator timings are single runs vs historical v7 runs, not controlled CPU-speed measurements.'])
 (root/'reports/RESULTS_V8.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
 lines=['# v8 官方結果：先預演溢出，再固定分核重排','','40 組全部使用未修改的官方評分器；全部核分配與 v7 相同。加速比為官方單核 cycles / 本方案 cycles。百分比均相對 v7，負值表示減少。','','| 核數 | v7 平均加速比 | v8 平均加速比 | cycles 合計變化 | 溢出合計變化 | 改變方案數 | 平均生成秒 |','|---:|---:|---:|---:|---:|---:|---:|']
 for c,s in report['by_cores'].items():lines.append(f'| {c} | {s["v7_mean_speedup"]:.6f}× | {s["mean_speedup"]:.6f}× | {s["cycles_change_percent"]:+.3f}% | {s["spill_bytes_change_percent"]:+.3f}% | {s["changed_plans"]} | {s["generation_mean_seconds"]:.3f} |')
 for c in range(2,6):
  lines+=['',f'## {c} 核','','| 用例 | v8 cycles | 加速比 | 總 COPY bytes | 溢出 bytes | v7 溢出 bytes | cycles 變化 | 生成秒 |','|---|---:|---:|---:|---:|---:|---:|---:|']
  for x in rows:
   if x['cores']==c:lines.append(f'| {x["case"]} | {x["cycles"]:,} | {x["speedup"]:.6f}× | {x["copy_bytes"]:,} | {x["spill_bytes"]:,} | {x["v7_spill_bytes"]:,} | {x["cycles_change_percent"]:+.3f}% | {x["generation_seconds"]:.3f} |')
 lines+=['','方案、result、trace、log 在 `runs/official_all/case_XX_nN/`；精確數值及雜湊見 [RESULTS_V8.json](RESULTS_V8.json)。生成秒數不含官方評分。','內部 cycles 非退步篩選使用既有估價模型，官方 cycles 仍可能退步；上表保留所有結果。']
 (root/'reports/RESULTS_V8_zh.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
 print(json.dumps({'summary':report['summary'],'by_cores':report['by_cores']},indent=2))
if __name__=='__main__':
 sys.stdout.reconfigure(encoding='utf-8');main()
