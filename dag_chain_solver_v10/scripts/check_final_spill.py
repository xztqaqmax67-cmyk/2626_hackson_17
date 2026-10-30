"""Check changed final plans; reuse exact-trace evidence only for identical inputs/plans/binary/source."""
from pathlib import Path
import json,hashlib,sys,os,subprocess
from check_official_spill import official_trace,compare

def read(p):return json.loads(p.read_text(encoding='utf-8'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,x):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(x,indent=2),encoding='utf-8')
def main():
 r=Path(__file__).resolve().parents[1];ref=r/'references/hackathon_20261030';out=r/'runs/spill_differential_final'
 sys.path.insert(0,str(r/'tests'));sys.path.insert(0,str(ref/'code'))
 import multicore_cut_evaluate_problem_2 as official
 prior=read(r/'runs/spill_differential/report.json');current=read(r/'runs/official_all/summary.json')
 assert prior['completed'] and prior['passed']==120 and current['completed']
 diag=r/'build_msvc/bin/spill_diagnostics.exe'
 assert sha(diag)==prior['metadata']['diagnostics_sha256']
 assert sha(r/'reports/rules_manifest.json')==prior['metadata']['official_manifest_sha256']
 for name,h in read(r/'reports/rules_manifest.json')['files'].items():assert sha(ref/name)==h
 for name,h in prior['metadata']['source_sha256'].items():assert sha(r/name)==h
 prev={(x['case'],x['cores']):x for x in prior['rows'] if x['group']=='public'}
 report={'completed':False,'previous_report_sha256':sha(r/'runs/spill_differential/report.json'),'official_run_sha256':sha(r/'runs/official_all/summary.json'),'diagnostics_sha256':sha(diag),'rows':[]}
 env=dict(os.environ,PYTHONUTF8='1',PYTHONDONTWRITEBYTECODE='1')
 for row in current['rows']:
  key=row['case'],row['cores'];graph=ref/'data'/f'{key[0]}.json';plan=r/row['plan'];old=prev[key]
  assert sha(graph)==row['input_sha256'] and sha(plan)==row['plan_sha256'] and row['status']=='ok'
  folder=out/f'{key[0]}_n{key[1]}';reused=old['plan_sha256']==row['plan_sha256'] and old['input_sha256']==row['input_sha256']
  if reused:
   cpp=r/'runs/spill_differential'/f'public_{key[0]}_n{key[1]}'/'cpp_trace.json'
   assert old['status']=='ok' and sha(cpp)==old['cpp_trace_sha256']
   trace=read(cpp)
  else:
   folder.mkdir(parents=True,exist_ok=True);expected=official_trace(official,read(graph),read(plan));save(folder/'official_trace.json',expected)
   cpp=folder/'cpp_trace.json';run=subprocess.run([str(diag),str(graph),str(plan),str(cpp)],capture_output=True,env=env,timeout=120)
   assert run.returncode==0,run.stderr.decode('utf-8',errors='replace')
   trace=read(cpp);compare(expected,trace)
  assert trace['valid'] and trace['spill_bytes']==row['data_movement_bytes']['spill_added_copy_bytes']
  assert trace['route_bytes']+trace['spill_bytes']==row['data_movement_bytes']['scheduled_copy_bytes']
  report['rows'].append({'case':key[0],'cores':key[1],'status':'ok','reused_identical_plan':reused,'plan_sha256':row['plan_sha256'],'cpp_trace':cpp.relative_to(r).as_posix(),'cpp_trace_sha256':sha(cpp),'spill_bytes':trace['spill_bytes'],'route_bytes':trace['route_bytes']})
  save(out/'report.json',report);print(key,'reused' if reused else 'new exact check',flush=True)
 report.update(completed=True,passed=40,reused=sum(x['reused_identical_plan'] for x in report['rows']),new_exact_checks=sum(not x['reused_identical_plan'] for x in report['rows']))
 save(out/'report.json',report);print(json.dumps({k:v for k,v in report.items() if k!='rows'}),flush=True)
if __name__=='__main__':sys.stdout.reconfigure(encoding='utf-8');main()
