# DAG Chain Solver v6 0.6.3 候选版

本版基于 v6.2，优先尝试有效的内存重排候选，并提供3秒暖启动方向筛查工具。
正式程序仍共用约15秒总预算，最多四个搜索工作，支持2～5核，默认4核。

## 构建与运行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
build/bin/npu_sched input.json -n 5 -o output/plan.json
```

Windows 完整交付包可直接运行 `npu_sched.exe`（Git 仓库保存源码，需按上面的命令构建）；`--version` 显示 `dag-chain-solver 0.6.3`。
输出只有 `node_to_subgraph` 和 `core_schedules`。正式程序无需 Python、数据文件、config
或历史方案，不读取评分器，不接受额外调参旗标。输入图与写者同核规则保持不变。

## 改动及实测

详见 [本轮搬运实验报告](reports/TRAFFIC_EXPERIMENTS_zh.md)。五核三个代表点完整15秒配对：
总cycles下降3.6031%，总COPY增加0.5146%；仅证明有周期收益，尚未证明整体降搬运。
07最终复测支持周期收益，但搬运变化不稳定。没有重跑全十点，不报告新十点平均加速比。
历史v6.2完整十点结果保留在 [前辈思路对比报告](reports/SENIOR_COMPARISON_zh.md)，不能算作v6.3成绩。
保留初始赢家不等于保证优于旧限时搜索。时间从读图前开始，但初始种子必须完整完成，
所以任意巨大输入不保证15秒硬截止。设置及规则区分见 `SEARCH_LIMITS_zh.md`。

## 验证

```sh
cmake -S . -B build -DCHAIN_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python tests/contracts.py --solver build/bin/npu_sched_quick --out runs/cli
python tests/rules_contracts.py --solver build/bin/npu_sched_quick --out runs/rules
```

本次MinGW GCC13.2直接编译：11组C++测试、CLI、26个图契约、16个合成图五核官方评估通过。
开发探针20次零预算契约调用通过。未在Linux或MSVC上重测，也未在本机执行CMake构建。

## 快速筛查

```sh
python scripts/run_search_probe.py --probe build/bin/search_probe --out runs/screen_new --seconds 3 --mode resume --cases 03 05 08 07
```

Windows工具名加`.exe`；使用上面的开发构建生成探针。默认读取 `runs/senior_15s` 的冻结方案，
可用 `--seed-root` 指定。3秒只计追加搜索，准备另计，**不是完整求解3秒**。候选有明显收益后再
用 `--mode full --seconds 15` 对少数点验证。正式程序仍从输入自行求解，不读取历史方案。
无收益的生命周期重排和搬运偏好策略在 `experiments/traffic/` 归档，不进入正式搜索。

完整包包含 `references/hackathon_20261030` 供开发复测。仅源码包不含官方数据；在原GitHub
仓库目录执行 `python scripts/prepare_reference.py` 可从仓库固定归档提取。

```sh
python tests/run_v5_synthetic.py --solver build/bin/npu_sched_quick --out runs/synthetic
python scripts/run_five_15s.py --solver build/bin/npu_sched --out runs/new_15s
python scripts/compare_senior.py --baseline reports/senior_validation/baseline.json --candidate runs/new_15s/summary.json --out reports/new_comparison.json
```

旧的 `summarize_five_15s.py` 专用于历史0.6.1与30秒版本比较；本次应使用 `compare_senior.py`。
交付包中 results 的summary路径字段保留原始测量来源；原始计划/结果按测试点目录随包保存。
正式源码打包仍可用 `python scripts/package.py`，只包含构建文件、src与说明，不含评测资料。
