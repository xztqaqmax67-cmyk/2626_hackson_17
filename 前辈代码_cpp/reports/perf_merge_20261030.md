# perf_optimization.rar 合并记录（2026-10-30）

已将压缩包 `optimization.patch` 的 6 项累计性能优化合入当前 `前辈代码_cpp/problem2/`，正式入口 `npu_sched` 与开发入口共享优化后的算法库。

压缩包基于较早的评分实现，本次以当前工程为基线逐项合并，保留链初解、新官方 90% 提前换出、多写者重命名/脏回写、写者同核校验、固定硬件和 90 秒正式预算。官方 Python 源码及测试输入未变更。压缩包内的旧评分代码、构建缓存、二进制和旧实验成绩未覆盖当前工程；历史 `submission/` 副本不属于这次原地维护目标。

## 合入的改动

| 改动 | 实现与复杂度收益 |
|---|---|
| JSON 构造和索引 | 唯一 ID 的 ordered JSON 直接预分配并追加，避免每次插入的线性查找；连续插入由 O(N²) 降为 O(N)。核归属用整数映射；Task 张量用有序数组和 ID 索引，保留原插入与替换顺序。 |
| 子图时间跨度 | 遍历时间线一次建立跨度表，替代每个子图重新扫描所有记录；由 O(BR) 降为 O(R log B + B log B)。 |
| 标量估价接口 | 搬运量、sketch 和生命周期的纯数值调用省去明细 JSON、区间排序和多余分配，保留明细接口供诊断。 |
| 排序评分预计算 | 候选生命周期评分从排序比较器中的重复调用改为每个候选计算一次；稳定排序及同分选择顺序保持。 |
| 结果共享与缓存 | 搜索行共享只读评分结果，缓存方案指纹和分核键，复用已计算的候选指纹；内存入口不再构造无需写盘的检查点。 |
| Pipe 定长数组 | 五条 Pipe 的时间和负载改为数组，预先索引算子的 Pipe，并移动候选结果以减少复制。 |

## 验证

Windows x64、MinGW GCC 16.1.0、C++17 Release；独立构建目录为 `build/perf_merge/`。

- 9 组回归全部通过：生成器、链不变量、性能语义、新官方评分、随机多核对照、固定评估预算搜索、命令行工作流、输入/硬件约束和链初解集成。
- 新增 `performance_semantics` 覆盖 2～5 核共 12 个方案，比较标量与明细计算、JSON 顺序及共享结果/缓存。
- 新官方回归覆盖 90% 软阈值、多写者脏回写/重命名、写者同核、拓扑检查及 2～5 核完整结果对照。
- 保存的 10 个四核链初解，合并前后完整评价结果的规范化 SHA-256、makespan、全部搬运统计均一致。
- 官方来源清单哈希通过；链实现、Step2/Step3、正式入口和配置与合并前逐字节一致。

合并期间另一项 screening proxy 实验也在修改同一工程。本次保留这些改动，并用独立构建目录避免产物冲突。默认 `legacy` 路径不启用实验代理；搜索对照测试只单独校验新诊断字段 `proxy_calls`，其余统计、历史、最终方案与原 Python 继续严格比较。

## 当前版本固定工作量计时

两个版本使用同一编译器和 Release 配置；串行交替运行相同方案，每个版本每例 3 次取中位数。表中是评分函数耗时，不含输入读取与结果哈希。测试期间工作区有其他任务活动，计时仅作为本机参考。

| 用例 | 合并前 / 秒 | 合并后 / 秒 | 加速 | 完整结果 |
|---|---:|---:|---:|---|
| case_00 | 0.5810 | 0.4593 | 1.265× | 一致 |
| case_01 | 0.0228 | 0.0220 | 1.035× | 一致 |
| case_02 | 0.0397 | 0.0370 | 1.073× | 一致 |
| case_03 | 1.3850 | 1.0147 | 1.365× | 一致 |
| case_04 | 1.2842 | 0.9239 | 1.390× | 一致 |
| case_05 | 6.4224 | 2.9339 | 2.189× | 一致 |
| case_06 | 0.5174 | 0.4055 | 1.276× | 一致 |
| case_07 | 3.7112 | 2.4523 | 1.513× | 一致 |
| case_08 | 3.9402 | 2.3726 | 1.661× | 一致 |
| case_09 | 3.7319 | 2.5523 | 1.462× | 一致 |
| 合计 | 21.6358 | 13.1734 | 1.642× | 10/10 一致 |

合计评分耗时减少 **39.1%**。这不是整段求解的加速比，也不是新一轮方案质量提升；未重新进行全用例 90 秒搜索或 Linux/MSVC 验收。压缩包的旧模型计时没有当作本次合并成绩。

## 历史复现命令

以下命令记录清理前的验证过程。当前版本已按要求移除测试目录及构建目标，命令仅供历史追溯。

在工程目录执行：

```powershell
cmake -S . -B build/perf_merge -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DPROBLEM2_TEST_PYTHON=C:/Users/max67/miniconda3/python.exe
cmake --build build/perf_merge --parallel 4
cmake --build build/perf_merge --target perf_runner
ctest --test-dir build/perf_merge --output-on-failure
```

`perf_runner` 仅显式构建，不进入默认提交目标。计时脚本需要先为合并前快照构建同一测试入口，然后执行：

```powershell
python tests/benchmark_perf_merge.py --before ../tmp/perf_merge_20261030/baseline_build/bin/perf_runner.exe --after build/perf_merge/bin/perf_runner.exe --inputs official_evaluator/data --plans output/chain_seed_review_20261030/chain_only_reference_4cores --out output/perf_merge_recheck --repeat 3
```

本次新编译的正式程序：`build/perf_merge/bin/npu_sched.exe`。

原始计时、完整结果哈希和二进制指纹见 [report.json](perf_merge_20261030/report.json)；来源压缩包与合并文件指纹见 [provenance.json](perf_merge_20261030/provenance.json)。
