"""Summarize completed supplied-evaluator runs without changing or rerunning them."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import statistics


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runs', type=Path, default=root/'runs/official_v3_1_20261030')
    args = parser.parse_args()
    runs = args.runs.resolve()
    multi = json.loads((runs/'problem2/summary.json').read_text(encoding='utf-8'))
    single = json.loads((runs/'singlecore/summary.json').read_text(encoding='utf-8'))
    assert len(multi['rows']) == 40 and len(single['rows']) == 10
    assert single.get('completed'), 'single-core batch has not completed'
    assert multi['config_sha256'] == single['config_sha256']
    assert multi['evaluator_source_sha256'] == single['evaluator_source_sha256']
    assert all(row['status'] == 'ok' for row in multi['rows']+single['rows']), \
        'retain failures and revise aggregation explicitly; never silently omit them'
    baseline = {row['case']: row for row in single['rows']}
    combined = []
    for row in multi['rows']:
        base = baseline[row['case']]
        assert row['input_sha256'] == base['input_sha256']
        assert sha(runs/'problem2'/row['artifacts']/'result.json') == row['result_sha256']
        assert sha(runs/'singlecore'/row['case']/'result.json') == base['result_sha256']
        original = row['data_movement_bytes']['original_graph_copy_bytes']
        added = row['data_movement_bytes']['added_copy_bytes']
        assert original == base['data_movement_bytes']['original_graph_copy_bytes']
        combined.append(dict(row, singlecore_makespan=base['makespan'],
                             speedup=base['makespan']/row['makespan'],
                             added_copy_ratio=added/max(1, original)))
    averages = {}
    for n in range(2, 6):
        selected = [row for row in combined if row['cores'] == n]
        assert len(selected) == 10
        averages[str(n)] = dict(cases=10, valid=10,
                               arithmetic_mean_speedup=statistics.mean(row['speedup'] for row in selected),
                               mean_added_copy_ratio=statistics.mean(row['added_copy_ratio'] for row in selected))
    mean_speedup = statistics.mean(row['speedup'] for row in combined)
    summary = dict(version='0.3.1', date='2026-10-30', scoring_performed=True,
                   official_simulation_run=True, valid=40, invalid=0,
                   baseline='supplied unmodified singlecore_evaluate.py',
                   final_organizer_score_claimed=False,
                   speedup_formula='singlecore_makespan / problem2_makespan',
                   average_formula='arithmetic mean over 10 cases and N=2..5 (40 equally weighted ratios)',
                   mean_speedup=mean_speedup, by_cores=averages,
                   mean_added_copy_ratio=statistics.mean(row['added_copy_ratio'] for row in combined),
                   config_sha256=multi['config_sha256'],
                   evaluator_source_sha256=multi['evaluator_source_sha256'],
                   generator_executable_sha256=multi['generator_executable_sha256'],
                   supplied_archive_sha256=sha(root.parent/'dag_chain_solver_v2/hackathon.zip'),
                   solver_source_sha256={p.relative_to(root).as_posix(): sha(p)
                                         for p in sorted((root/'src').glob('*')) if p.is_file()},
                   baseline_rows=single['rows'], rows=combined)
    (root/'reports/official_evaluation_v3_1_20261030.json').write_text(
        json.dumps(summary, indent=2), encoding='utf-8')
    for source, target in [('problem2', 'official_problem2_v3_1_20261030.json'),
                           ('singlecore', 'official_singlecore_v3_1_20261030.json')]:
        shutil.copyfile(runs/source/'summary.json', root/'reports'/target)
    lines = [
        '# v0.3.1：附帶官方評分器測試結果', '',
        '日期：2026-10-30。問題 2／場景 B，10 個公開用例 × 2、3、4、5 核，**40/40 通過完整評估**。',
        '另以同包 `singlecore_evaluate.py` 完成 10/10 單核基準評估。沒有修改求解器、官方程式、資料或配置來取得本次結果。', '',
        f'**平均加速比：{mean_speedup:.6f}×**。每組加速比為單核基準 Makespan ÷ 該方案 Makespan；',
        '先對每個核數的 10 個比值取算術平均，再對四個核數取平均，等同 40 個比值等權平均。',
        '這是以附帶單核程式為基準的本地測試結果。提交要求指出最終單核基準由組委會決定，因此不宣稱是最終排行榜分數。', '',
        '| 可用核心數 | 通過 | 平均加速比 | 平均額外搬運量／原圖搬運量 |',
        '| --- | --- | --- | --- |',
    ]
    for n, value in averages.items():
        lines.append(f"| {n} | 10/10 | {value['arithmetic_mean_speedup']:.6f}× | {value['mean_added_copy_ratio']:.4f} |")
    lines += ['', '## 逐例 Makespan（cycles）', '',
              '| 用例 | 附帶單核基準 | 2 核 | 3 核 | 4 核 | 5 核 |',
              '| --- | --- | --- | --- | --- | --- |']
    lookup = {(row['case'], row['cores']): row for row in combined}
    for case, value in sorted(baseline.items()):
        numbers = [value['makespan']]+[lookup[(case, n)]['makespan'] for n in range(2, 6)]
        lines.append('| '+case+' | '+' | '.join(f'{number:,}' for number in numbers)+' |')
    lines += ['', '## 逐例加速比', '', '| 用例 | 2 核 | 3 核 | 4 核 | 5 核 |', '| --- | --- | --- | --- | --- |']
    for case in sorted(baseline):
        lines.append('| '+case+' | '+' | '.join(f"{lookup[(case, n)]['speedup']:.4f}×" for n in range(2, 6))+' |')
    lines += ['', '可用核心數不要求全部使用，單核候選仍可獲選；增加可用核心也不保證 Makespan 單調下降。',
              '上述加速比描述 NPU 排程的模擬執行時間，不是 C++ 求解器多執行緒的牆鐘加速比。', '',
              '附帶單核評估器對 case_08 回傳 `partition_added_copy_bytes=-7276544`，已原樣保存。',
              '該欄位是 Task 圖 COPY 流量減原圖 COPY 流量的差值，本報告不截零；它不參與 Makespan 加速比計算。', '',
              '## 執行與追溯', '',
              '- 多核入口：原包 `code/multicore_cut_evaluate_problem_2.py`。',
              '- 單核入口：原包 `code/singlecore_evaluate.py`；它把所有計算操作合成單一子圖，再執行官方單核流程。',
              '- 配置：原包 `data/config.txt`，L1=1572864、UB=131072、DDR=60、跨核等待=1000。',
              '- 使用前次生成並通過雜湊核對的 v0.3.1 MSVC 方案，未改寫方案以適應評分結果。',
              '- 官方來源、輸入、配置、方案、執行檔及結果均記錄 SHA-256。',
              '- `reports/official_evaluation_v3_1_20261030.json`：完整統計、40 組搬運量與逐例加速比。',
              '- `reports/official_problem2_v3_1_20261030.json`、`reports/official_singlecore_v3_1_20261030.json`：兩種批次的原始摘要。',
              '- `runs/official_v3_1_20261030/problem2/case_XX_nN/`：每組 result、Perfetto trace、日誌、stdout、stderr、執行命令。',
              '- `runs/official_v3_1_20261030/singlecore/case_XX/`：對應單核產物。', '',
              '研究批次器為 `scripts/run_official_problem2.py` 與 `scripts/run_official_singlecore.py`；',
              '`scripts/summarize_official.py` 只讀取已完成結果計算本報告。它們不隨最小提交包交付。',
              '評分器的開發批次上限設為每次 600 秒，與提交求解器的 90 秒生成限制分開；本次沒有超時。', '',
              '本次驗證限於提供的 10 個公開圖，沒有隱藏用例或 Linux 執行結果。', '']
    (root/'OFFICIAL_EVALUATION_v3_1.md').write_text('\n'.join(lines), encoding='utf-8')
    print(json.dumps({'mean_speedup': mean_speedup, 'by_cores': averages}, indent=2))


if __name__ == '__main__':
    main()
