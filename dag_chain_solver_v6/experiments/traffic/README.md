# 搬运优化的固定候选短测

这是开发实验目录，**不参与正式求解器构建，也不进入默认搜索**。它保存了本轮未采用的
“单算子生命周期局部重排”方向，以及可用于其他搬运方向的快速筛查工具。

`lifetime.cpp` 固定分核，按张量大小与同核存活跨度选择热点；在依赖允许范围内移动一个
算子，提前最后使用者、推迟首个使用者，或移走区间内无关的大输出算子。最多提出24个
候选，保持其他算子的相对顺序。生命周期变短不保证真实溢出下降，仍需完整评估。

## 本轮结果与采用决定

初轮从已保存的 v6.2 五核方案出发，对 case03、05、08、07、09 共筛查44个新候选。
内部筛查合计 **4.68秒**；随后包括基准在内的15次官方复评合计 **52.45秒**。

| 测试点 | 官方复评结果 |
|---|---|
| 03、07 | 所选候选均持平 |
| 05、08 | 所选候选略退步 |
| 09 | 最佳 cycles 从3,782,168降到3,781,308（−0.0227%），COPY 从164,607,744降到164,570,624（−0.0225%） |

因此本轮不把该单算子邻域加入生产搜索，也不继续花15秒逐点扩大验证。这个结论仅针对
当前局部构造，不代表块级生命周期优化没有空间。初轮完整摘要按原字节保存在
[initial_results.json](initial_results.json)，原计划、screen及官方trace仍在本次交付的实验记录中。

初轮使用每点最多3个新候选且重新评分基准。归档脚本现在默认每点只官方复评 **1个新候选**，
并在完整哈希证据一致时复用基准评分；这些效率改动没有追溯修改上述耗时或初轮数据。

## 编译

在 `dag_chain_solver_v6` 根目录执行，使用C++17编译器。下面直接编译源文件，不依赖已有
构建目录、对象文件或本机绝对路径；GCC/MinGW可运行此命令，`.exe` 后缀在Linux亦可执行。

```sh
g++ -std=c++17 -O2 -pthread -I src -I experiments/traffic experiments/traffic/fixed_candidates.cpp experiments/traffic/lifetime.cpp src/chain.cpp src/refine.cpp src/regions.cpp src/slack.cpp src/anytime.cpp src/tensor_neighborhood.cpp -o experiments/traffic/fixed_candidates.exe
g++ -std=c++17 -O2 -pthread -I src -I experiments/traffic experiments/traffic/lifetime_tests.cpp experiments/traffic/lifetime.cpp -o experiments/traffic/lifetime_tests.exe
experiments/traffic/lifetime_tests.exe
```

MinGW如需独立于编译器运行库运行，可额外增加 `-static -static-libgcc -static-libstdc++`。
本轮原型已通过专项测试。归档脚本另通过Python语法检查、根目录定位和9项缓存入口检查，
覆盖正常复用、不同路径但相同证据，以及输入、方案、配置、源码、结果和指标不匹配的拒绝。
归档后未重新编译原型或重复官方大测试；这组轻量验证不计入原始短测结果。

## 短测

准备官方材料并将完整 v6.2 测试记录放到 `runs/senior_15s`，或用 `--plans` 指定包含
`summary.json` 及每点 `plan_n5.json`、`result.json` 的目录。Git中的精简摘要本身不足以复用。

```sh
python scripts/prepare_reference.py
python experiments/traffic/run_fixed_candidates.py --out runs/fixed_lifetime_new --family lifetime --cases 03 05 08 07 09
```

- `--family memory` 测全图内存偏好重排；`consumer` 测消费者迁移；`all` 合并方向。
- `--official-limit 3` 扩大前沿复评，默认1以降低首轮成本。
- `--stage screen` 只生成及内部精评；`--stage evaluate` 复用同目录筛查结果再官方评分。
- `--fresh-baseline` 强制重新官方评分基准，跳过评分缓存。
- 更换脚本、驱动程序、候选上限等条件时必须使用新的输出目录。

工具从既有方案出发，属于**暖启动方向筛查**，不能作为从原始输入开始的求解耗时或正式
加速比报告。预处理、候选构造、内部精评、写出和官方评分分别计时。短测有希望的方向仍需
在完整初始搜索与统一预算下对照，尤其需检查未参与方向筛查的测试点。

## 基准评分缓存的条件

缓存仅适用于候选0。脚本同时核对：原记录完整成功且为五核、输入哈希、原始与重新写出
方案的**字节哈希**、config哈希、全部官方Python源码哈希、官方结果文件哈希，以及结果
中的规则参数、cycles和搬运分类。路径相同不构成复用条件；仅搬目录不会破坏可靠性，
但任何内容或证明缺失都会退回重新评分。新候选始终官方复评，不使用基准结果替代。
复用结果明确记录 `reused_official=true`、来源摘要哈希和本次官方耗时0，不伪造新测量。
