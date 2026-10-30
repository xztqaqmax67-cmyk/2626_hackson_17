# DAG Chain Solver v6 0.6.2

本版基于 GitHub v6 0.6.1，吸收前辈 C++ 的多路线搜索与消费者组迁移/负载补偿思路。
正式程序仍共用约15秒总预算，最多四个搜索工作，支持2～5核，默认4核。

## 构建与运行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
build/bin/npu_sched input.json -n 5 -o output/plan.json
```

Windows 完整交付包可直接运行 `npu_sched.exe`（Git 仓库保存源码，需按上面的命令构建）；`--version` 显示 `dag-chain-solver 0.6.2`。
输出只有 `node_to_subgraph` 和 `core_schedules`。正式程序无需 Python、数据文件、config
或历史方案，不读取评分器，不接受额外调参旗标。输入图与写者同核规则保持不变。

## 改动及实测

详见 [对比报告](reports/SENIOR_COMPARISON_zh.md) 与 [验证记录](reports/senior_validation/)。同编译器、同15秒、五核十点首轮：
平均单核加速比 2.249665× → 2.281835×（提高1.4300%）；总cycles降低0.8113%。
五点改善、三点持平、两点退步；总搬运量增加0.9383%。这是有取舍的小幅改善。
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

本次GCC13.2编译、十组C++测试、CLI、26个图契约、64个合成图官方评估，以及十个正式
五核官方评估通过。未在Linux或MSVC上重测本次改动。更旧版本的历史报告不属于0.6.2验证。

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
