# 问题二算法附件

本程序求解场景 B，同一核心上的全部子图合并为一个 Task。需要 Linux、Python 3.11 或更新版本，仅使用 Python 标准库，无需安装第三方库。

## 运行

解压后，在包含 `problem2/` 的目录执行。将题目原始计算图放在 `inputs/` 中：

```bash
python -B problem2/main.py inputs/case_001.json -n 4 \
  --config problem2/config.txt --output-dir output/problem2/cores_4 \
  --export-evaluation
```

输入路径和文件名可以任意指定。省略 `--config` 时，读取输入计算图同目录的 `config.txt`。本附件所带配置与题目原始配置完全一致，正式实验应使用这一固定配置。

输出目录将包含：

- `case_001_multicore_res.json`：仅包含 `node_to_subgraph`、`core_schedules` 两个字段的标准方案。
- `case_001_problem_2_res.json`：官方数值评估结果。
- `case_001_problem_2_trace.json`：Perfetto/Chrome 时间线。
- `case_001_problem_2_log.txt`：官方格式的简短日志。

省略 `--export-evaluation` 只导出方案。省略输出参数时，方案保存至输入图所在目录。输出不能覆盖输入、配置或写入本题源码目录；不同核数应使用不同输出目录。

## 参数和求解时间

- `-n`：目标核心数，支持 1–5。1 核采用官方整图单核方案并实际评价，不进行优化。
- 2–5 核默认目标时间依次为 60、120、180、240 秒，1 核默认 600 秒。可用 `--time-limit` 显式指定。
- `--feasibility-grace`：目标时间内尚无有效解时允许的首解延时，默认 60 秒、最大 120 秒；已有有效解不会延时继续优化。1 核不延时。
- `--work-dir`：可选，保留日志、检查点等辅助记录，须指定源码目录外的空目录，并与最终输出目录分开。默认使用自动清理的临时目录。
- `--help`：显示全部参数。

限时搜索会受机器速度和负载影响，再次运行不保证获得完全相同的最好方案；同一方案使用相同官方配置复评应得到相同指标。

## 官方独立复评

```bash
python -B problem2/official_code/multicore_cut_evaluate_problem_2.py \
  inputs/case_001.json output/problem2/cores_4/case_001_multicore_res.json \
  --config problem2/config.txt \
  -o output/problem2/cores_4/check_res.json \
  --trace-output output/problem2/cores_4/check_trace.json \
  --log-output output/problem2/cores_4/check_log.txt
```

`official_code/` 保留题目原始程序，未修改评估算法、指标或配置规则。

## 全部用例

将官方 100 个计算图放入 `inputs/` 后，可逐个输入和核数调用单图入口：

```bash
for graph in inputs/case_*.json; do
  for n in 1 2 3 4 5; do
    python -B problem2/main.py "$graph" -n "$n" \
      --config problem2/config.txt --output-dir "output/problem2/cores_$n" \
      --export-evaluation || exit 1
  done
done
```

以上命令分别独立求解各配置。独立调用不会共享整例时间上限，也不会隐式读取历史结果。需要复用已有方案时，必须显式传入下面列出的参数；来源校验和官方重评机制均保留。

## 可选的方案复用

`--inherit-checkpoint FILE` 显式继承同题、同一输入的较低核数 `best_checkpoint.json`，在目标核数重新官方评价后参与保优。检查点通过输入、配置和官方代码的 SHA-256 校验来源，移动文件不会改变来源身份。

`--search-profile full` 可启用较宽的搜索配置；默认 `compact` 使用精简配置。

## 提交内容

本目录可整体作为一份代码附件，包含算法源码、原样官方评估程序、固定配置和本说明。运行依赖均在本目录内；输入计算图由题目材料提供。运行摘要只记录文件名，检查点来源以文件名和哈希标识，不记录调用机器的目录前缀。标准方案只含题面规定的两个字段。

算法之外的旧实验脚本、历史路径、编译缓存及测试产物未包含在本附件中。官方评估器按题目原样保留；如将自行保存的错误日志加入附件，应另行检查其内容。
