import csv
import json
import statistics
from pathlib import Path

ROOT=Path(__file__).resolve().parent
STAGES=['v0_baseline','v1_json','v2_spans','v3_scalar','v4_scores','v5_cache','v6_pipes']
LABELS=['原版','JSON 构造与索引','+ 单遍汇总子图跨度','+ 数值接口省略明细','+ 排序代价预计算','+ 缓存及减少复制','+ 流水线定长数组']

def read(path):
    return json.loads(path.read_text(encoding='utf-8')) if path.exists() else None

def cell(x, places=3):
    return '—' if x is None else f'{x:.{places}f}'

def main():
    records={}
    for stage in STAGES:
        for mode in ('fixed','search','single','replay','verify'):
            for f in (ROOT/'results'/stage/f'{mode}_20s_n4').glob('*/0/summary.json'):
                records[stage,mode,f.parents[1].name]=read(f)
    references={case:r['makespan'] for (s,m,case),r in records.items()
                if s=='v6_pipes' and m=='single' and 'makespan' in r}
    searchrows=[]
    for stage in STAGES:
        for case in sorted(p.stem for p in (ROOT.parent/'hackathon_data').glob('*.json')):
            r=records.get((stage,'search',case))
            if not r: continue
            report=r.get('report',{})
            b=records.get(('v0_baseline','search',case),{}).get('report',{})
            move=report.get('data_movement_bytes',{})
            oldmove=b.get('data_movement_bytes',{})
            t=report.get('makespan'); t0=b.get('makespan'); ref=references.get(case)
            d=move.get('scheduled_copy_bytes'); d0=oldmove.get('scheduled_copy_bytes')
            searchrows.append(dict(stage=stage,case=case,cores=4,budget=20,
                wall_seconds=r.get('wall_seconds'),solve_seconds=r.get('solve_seconds'),
                makespan=t,total_ddr_bytes=d,added_copy_bytes=move.get('added_copy_bytes'),
                spill_bytes=move.get('spill_added_copy_bytes'),
                local_singlecore=ref,local_speedup=ref/t if ref and t else None,
                makespan_reduction_pct=100*(t0-t)/t0 if t0 and t is not None else None,
                ddr_reduction_pct=100*(d0-d)/d0 if d0 and d is not None else None,
                evaluations=report.get('stats',{}).get('full_evaluations'),
                termination=report.get('termination'),exit_code=r.get('exit_code')))
    if searchrows:
        with (ROOT/'search_results.csv').open('w',newline='',encoding='utf-8-sig') as out:
            writer=csv.DictWriter(out,fieldnames=list(searchrows[0]));writer.writeheader();writer.writerows(searchrows)
    fixedrows=[]
    for (stage,mode,case),r in records.items():
        if mode!='fixed' or 'error' in r: continue
        b=records['v0_baseline','fixed',case]
        fixedrows.append(dict(stage=stage,case=case,evaluation_seconds=r['evaluation_seconds'],
            evaluation_speedup=b['evaluation_seconds']/r['evaluation_seconds'],
            full_result_identical=b['result_sha256']==r['result_sha256'],
            **{k:r[k] for k in ('fingerprint_seconds','route_seconds','sketch_seconds','lifetime_seconds')}))
    if fixedrows:
        with (ROOT/'fixed_results.csv').open('w',newline='',encoding='utf-8-sig') as out:
            writer=csv.DictWriter(out,fieldnames=list(fixedrows[0]));writer.writeheader();writer.writerows(fixedrows)
    lines=['# C++ 场景 B 逐项性能优化实验','',
        '测试输入：`E:/hackthon/hackathon_data`，共 10 个用例。4 核；搜索预算每次 20 秒（不含图预处理与写出）；硬件参数 L1=1572864、UB=131072、DDR=60、跨核等待=1000。',
        '', '基线为用户指定 C++ 实现，所有阶段累计改动。原目录与数据不修改。使用独立 CMake Release 构建（本机 MinGW GCC 13.2.0），测试进程串行运行，计时期间不并行编译。',
        '', '兼容性：COPY 的 cycles=-1 保持为占位符；PIPE_FIX 作为第五条独立流水线。评价模型使用原 C++ 实现，未改变这些规则。',
        '', '## 各阶段汇总','',
        '| 阶段 | 固定方案评价加速 | 平均本地单核加速比 | 总 DDR / GiB | 完整评价次数 |',
        '|---|---:|---:|---:|---:|']
    base_time=sum(r['evaluation_seconds'] for r in fixedrows if r['stage']=='v0_baseline')
    for stage,label in zip(STAGES,LABELS):
        sr=[r for r in searchrows if r['stage']==stage]
        fr=[r for r in fixedrows if r['stage']==stage]
        if not sr: continue
        speed=[r['local_speedup'] for r in sr if r['local_speedup'] is not None]
        elapsed=sum(r['evaluation_seconds'] for r in fr)
        total=sum(r['total_ddr_bytes'] or 0 for r in sr)
        lines.append(f'| {stage}: {label} | {cell(base_time/elapsed if elapsed else None)}× | {cell(statistics.mean(speed) if speed else None)}× | {total/2**30:.4f} | {sum(r["evaluations"] or 0 for r in sr)} |')
    lines+=['','固定方案评价加速 = 基线评价总耗时 / 本阶段评价总耗时；此表统计 9 个可执行固定方案。case_09 的人为连续划分方案在基线中也死锁，另用基线搜索产出的有效方案回放核验。',
        '', '本地单核加速比使用同一场景 B 模型、全部计算放同一核的本地参考时间；不是组委会尚未提供的正式单核基准。总 DDR 为 scheduled_copy_bytes，另有 added_copy_bytes 与 spill_bytes 见 CSV。',
        '', '## 最终阶段逐例结果','',
        '| 用例 | 基线周期 | 优化后周期 | 周期减少 | 本地加速比（前→后） | DDR 减少 |',
        '|---|---:|---:|---:|---:|---:|']
    for r in searchrows:
        if r['stage']!='v6_pipes':continue
        b=next(x for x in searchrows if x['stage']=='v0_baseline' and x['case']==r['case'])
        lines.append(f'| {r["case"]} | {cell(b["makespan"],0)} | {cell(r["makespan"],0)} | {cell(r["makespan_reduction_pct"],2)}% | {cell(b["local_speedup"])}→{cell(r["local_speedup"])} | {cell(r["ddr_reduction_pct"],2)}% |')
    lines+=['','## 方法与限制','',
        '- 固定方案实验用于检验代码等价性：比较完整评价结果的规范化 SHA-256，并比较方案指纹、搬运估计、时间估计及生命周期分数。',
        '- 20 秒搜索实验使用原搜索策略与 compact 参数。其路线调度和停止条件依赖真实耗时，因此速度变化可能改变探索路径，单次质量结果不保证逐阶段单调。',
        '- 搜索时间是协作式预算，实际墙钟时间见 CSV，可能因预处理、收尾或一次操作跨过截止点而超过 20 秒。这些实验不等价于正式 90 秒验收。',
        '- 小幅计时差异可能属于系统噪声；完整评价速度主要由前两阶段改善，后续主要影响候选生成与筛选。',
        '', '## 文件','',
        '- `source/`：最终优化源码，包含可选测试入口；`build/bin/npu_sched.exe` 为正式接口程序。',
        '- `versions/`：每阶段源码与可执行文件快照。',
        '- `results/`：请求、完整搜索统计、输出方案与每次计时。',
        '- `search_results.csv`、`fixed_results.csv`：可直接用表格工具分析的数据。',
        '- `run_suite.py`、`check_stage.py`、`summarize.py`：复现实验与检查脚本。','']
    long_rows=[]
    for stage in ('v0_baseline','v6_pipes'):
        for f in (ROOT/'results'/stage/'search_80s_n4').glob('*/0/summary.json'):
            r=read(f); rep=r.get('report',{}); movement=rep.get('data_movement_bytes',{})
            long_rows.append(dict(stage=stage,case=r['case'],wall_seconds=r['wall_seconds'],
                makespan=rep.get('makespan'),total_ddr_bytes=movement.get('scheduled_copy_bytes'),
                added_copy_bytes=movement.get('added_copy_bytes'),
                evaluations=rep.get('stats',{}).get('full_evaluations')))
    if long_rows:
        with (ROOT/'long_budget_results.csv').open('w',newline='',encoding='utf-8-sig') as out:
            writer=csv.DictWriter(out,fieldnames=list(long_rows[0]));writer.writeheader();writer.writerows(long_rows)
        lines+=['## 80 秒搜索预算补充对照','',
            '| 用例 | 基线周期 | 最终周期 | 周期减少 | 基线/最终评价次数 | DDR 减少 |',
            '|---|---:|---:|---:|---:|---:|']
        for new in long_rows:
            if new['stage']!='v6_pipes':continue
            old=next((r for r in long_rows if r['stage']=='v0_baseline' and r['case']==new['case']),None)
            if not old:continue
            reduction=100*(old['makespan']-new['makespan'])/old['makespan']
            ddr=100*(old['total_ddr_bytes']-new['total_ddr_bytes'])/old['total_ddr_bytes']
            lines.append(f'| {new["case"]} | {old["makespan"]:.0f} | {new["makespan"]:.0f} | {reduction:.2f}% | {old["evaluations"]}/{new["evaluations"]} | {ddr:.2f}% |')
        lines+=['','这组补充实验选取最大计算图 case_05、短预算敏感的 case_07 和含多种流水线的 case_08；预算为搜索内部 80 秒，实际进程时间记录在 CSV。','']
    (ROOT/'report.md').write_text('\n'.join(lines),encoding='utf-8')
    print('Wrote report.md, search_results.csv, fixed_results.csv')

if __name__=='__main__': main()
