import csv
import json
from pathlib import Path
import statistics

ROOT=Path(__file__).resolve().parent
STAGES=['v0_baseline','v1_json','v2_spans','v3_scalar','v4_scores','v5_cache','v6_pipes']
LABELS=['原版','JSON 构造/索引','单遍汇总跨度','数值接口去明细','排序评分预计算','共享结果/缓存','Pipe 定长数组']

def load(p):return json.loads(p.read_text(encoding='utf-8'))
def fmt(v,d=3):return '—' if v is None else f'{v:.{d}f}'
def csvwrite(name,rows):
    if rows:
        with (ROOT/name).open('w',encoding='utf-8-sig',newline='') as stream:
            writer=csv.DictWriter(stream,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)

def main():
    official=load(ROOT/'latest_official/scores.json')
    scores={(r['case'],r['source'].replace('\\','/')):r for r in official}
    refs={r['case']:r.get('makespan') for r in official if r['source']=='singlecore' and r['status']=='ok'}
    rows=[]
    for stage in STAGES:
        for budget in (20,80):
            for f in sorted((ROOT/'results'/stage/f'search_{budget}s_n4').glob('*/0/summary.json')):
                r=load(f);report=r.get('report',{})
                source=(f.parent/'plan.json').relative_to(ROOT).as_posix()
                new=scores.get((r['case'],source),{})
                movement=new.get('data_movement_bytes',{})
                t=new.get('makespan');single=refs.get(r['case'])
                rows.append(dict(stage=stage,case=r['case'],budget=budget,cores=4,
                    wall_seconds=r['wall_seconds'],solve_seconds=r.get('solve_seconds'),
                    evaluations=report.get('stats',{}).get('full_evaluations'),
                    internal_old_makespan=report.get('makespan'),official_status=new.get('status','pending'),
                    official_makespan=t,singlecore_reference=single,
                    speedup=single/t if single is not None and t else None,
                    scheduled_copy_bytes=movement.get('scheduled_copy_bytes'),
                    added_copy_bytes=movement.get('added_copy_bytes'),
                    original_graph_copy_bytes=movement.get('original_graph_copy_bytes'),
                    spill_added_copy_bytes=movement.get('spill_added_copy_bytes'),
                    error=new.get('error','')))
    csvwrite('latest_scores.csv',rows)
    fixed=[]
    for stage in STAGES:
        for case in sorted((ROOT.parent/'hackathon_data').glob('*.json')):
            runs=[load(f) for f in (ROOT/'results'/stage/'fixed_20s_n4'/case.stem).glob('*/summary.json')]
            valid=[r for r in runs if r.get('exit_code')==0 and 'evaluation_seconds' in r]
            if valid:
                fixed.append(dict(stage=stage,case=case.stem,repetitions=len(valid),
                    median_eval_seconds=statistics.median(r['evaluation_seconds'] for r in valid)))
    csvwrite('fixed_medians.csv',fixed)
    times={s:sum(r['median_eval_seconds'] for r in fixed if r['stage']==s) for s in STAGES}
    lines=['# C++ 运行效率优化与最新官方评分器复评','',
        '**最终质量指标以 `E:/hackthon/hackathon_new/hackathon/code` 为准。此前旧内部模型的质量数字已被本报告替代。**','',
        '输入为 `E:/hackthon/hackathon_data` 的 10 个计算图；4 核；最终参数为 L1=1572864、UB=131072、DDR=60、跨核等待=1000。全套 7 个累计版本各使用 20 秒搜索预算；另对 3 个大图进行原版/最终版 80 秒搜索对照。',
        '', '构建：Windows / MinGW GCC 13.2.0，CMake Release。所有计时任务串行运行，计时期间不并行编译。原源码目录、输入数据、最新官方评分器均未修改。',
        '', '## 必须区分的两种结果','',
        '- **代码效率**：原版与优化版执行相同内部估价/模拟工作，比较耗时及完整结果一致性。这保留原算法内部模型，用于隔离代码优化效果。',
        '- **方案质量**：对保存的输出 JSON 使用最新官方 Python 评分器重新评价，记录合法性、makespan、总/额外 DDR 搬运量。单核参考由该目录的 `singlecore_evaluate.py` 接口计算。',
        '- 最新规则新增同 tensor 写者同核、90% 容量提前换出、多写者换出后重新写回及写者重命名。当前 C++ 搜索内部模型尚未移植这些规则；它可以作为启发式估价，但内部数值不能当作最新官方成绩，未经新评分器验证的其他输出不能保证有效。',
        '', '## 逐项优化（累计版本，20 秒搜索）','',
        '| 版本 | 改动 | 同工作量内部评价加速 | 官方有效方案 | 平均官方参考加速比 | 总 DDR / GiB | 内部评价次数 |',
        '|---|---|---:|---:|---:|---:|---:|']
    for stage,label in zip(STAGES,LABELS):
        items=[r for r in rows if r['stage']==stage and r['budget']==20]
        valid=[r for r in items if r['official_status']=='ok']
        ready=len(items)==10 and all(r['official_status']!='pending' for r in items) and len(refs)==10
        mean=sum(r['speedup'] or 0 for r in items)/10 if ready else None
        total=sum(r['scheduled_copy_bytes'] or 0 for r in valid)/2**30 if ready else None
        lines.append(f'| {stage} | {label} | {fmt(times[STAGES[0]]/times[stage] if times[stage] else None)}× | {len(valid)}/10 | {fmt(mean)}× | {fmt(total,4)} | {sum(r["evaluations"] or 0 for r in items)} |')
    lines+=['','内部评价加速按 9 个可执行固定方案的评价耗时总和计算；如有重复运行，逐例取中位数。第 10 个固定划分在原版也死锁，另外使用基线的有效方案验证，各版本完整评价结果一致。','',
        '## 最终版与基线逐例比较（最新官方，20 秒搜索）','',
        '| 用例 | 原版周期 | 最终周期 | 周期减少 | 参考加速比（原→最终） | 总 DDR 减少 |',
        '|---|---:|---:|---:|---:|---:|']
    for new in [r for r in rows if r['stage']=='v6_pipes' and r['budget']==20]:
        old=next(r for r in rows if r['stage']=='v0_baseline' and r['case']==new['case'] and r['budget']==20)
        t0,t=old['official_makespan'],new['official_makespan'];d0,d=old['scheduled_copy_bytes'],new['scheduled_copy_bytes']
        improvement=100*(t0-t)/t0 if t0 and t is not None else None
        traffic=100*(d0-d)/d0 if d0 and d is not None else None
        lines.append(f'| {new["case"]} | {fmt(t0,0)} | {fmt(t,0)} | {fmt(improvement,2)}% | {fmt(old["speedup"])}→{fmt(new["speedup"])} | {fmt(traffic,2)}% |')
    lines+=['','负的 DDR 减少比例表示搬运量增加；代码加速不会保证搜索结果的所有指标同时改善。','',
        '## 80 秒搜索补充对照（最新官方）','',
        '| 用例 | 原版周期 | 最终周期 | 周期减少 | 总 DDR 减少 | 完整评价次数（原→最终） |',
        '|---|---:|---:|---:|---:|---:|']
    for new in [r for r in rows if r['stage']=='v6_pipes' and r['budget']==80]:
        old=next(r for r in rows if r['stage']=='v0_baseline' and r['case']==new['case'] and r['budget']==80)
        t0,t=old['official_makespan'],new['official_makespan'];d0,d=old['scheduled_copy_bytes'],new['scheduled_copy_bytes']
        improvement=100*(t0-t)/t0 if t0 and t is not None else None
        traffic=100*(d0-d)/d0 if d0 and d is not None else None
        lines.append(f'| {new["case"]} | {fmt(t0,0)} | {fmt(t,0)} | {fmt(improvement,2)}% | {fmt(traffic,2)}% | {old["evaluations"]}→{new["evaluations"]} |')
    lines+=['','## 验证与限制','',
        '- 160 项小图与 Python 参考实现的生成、边界图、完整评分、生命周期、邻域和激活对照通过（这是纯代码优化的旧模型等价性检查）。',
        '- 最终 10 个短预算方案已由保存的原版 C++ 评价器独立回放，完整结果一致；最新官方评价的状态另见上表及 `latest_official/scores.json`。',
        '- 20/80 秒为搜索内部预算，不含图预处理、写出等；实际进程时间见 CSV。协作式超时可能略微超出预算，未据此宣称完成正式 90 秒全平台验收。',
        '- 搜索路线、阶段切换及预留时间依赖实测耗时，所以各版本的解质量不是单调函数；单次结果存在时间门槛波动。',
        '- 此次只比较 4 核，不把结果冒充 N=2..5 的完整比赛总分。',
        '', '## 交付文件','',
        '- `source/`：优化源码；`build/bin/npu_sched.exe`：已编译程序。',
        '- `optimization.patch`：相对基线的性能改动。',
        '- `versions/`：各阶段源码和二进制快照；`results/`：各阶段原始统计和方案。',
        '- `latest_scores.csv`：最新官方逐例质量、耗时、搬运量及错误；`fixed_medians.csv`：固定工作量计时。',
        '- `latest_official/provenance.json`：最新评分器代码/配置哈希；`latest_official/scores.json`：复评原始记录。',
        '- `evaluate_latest.py`、`summarize_latest.py`：官方复评与本报告复现脚本。','']
    (ROOT/'report.md').write_text('\n'.join(lines),encoding='utf-8')
    print('Updated authoritative report.md and latest_scores.csv')

if __name__=='__main__':main()
