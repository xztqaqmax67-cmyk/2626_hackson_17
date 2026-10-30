# v10 提交合规只读审计

审计对象：GitHub `main` 提交 `b709f70305e924a4c3f3a359264262d1b8b9e525` 下 `dag_chain_solver_v10`，程序版本 **0.10.1**。本次通过 GitHub connector 读取固定提交全部 `src/`、CMake、README、打包脚本和验证摘要；没有修改 v10、编译或运行基准。行号按该固定提交计算。

依据：[提交要求文档 v3.0](C:/Users/max67/OneDrive/文档/ChatGPT/Hackson/材料/提交要求文档.md)。对应规则为 §1.2 交付边界、§2.3 README、§2.4 禁止内容、§3 工程与第三方、§4 CLI/退出码/运行、§5.2 固定参数。

## 结论

**v10 不能原样作为严格合规提交包。** 它明确包含和调用官方评估器 Step 1/2/3 与 Scene B 的移植实现，并有 CPython 实现来源说明但缺少第三方登记。其主要新增选优机制依赖这些模块。仅关闭功能开关、修改名称或删除“移植”注释不能解决包内仍含评估器副本的问题。最终候选仍为 **0.8.3（基于 0.8.2 的整理版）**；本次吸收的是审查结论与后续优化思路，不是算法融合。

## 关键发现与最小处理

| 优先级 | 证据 | 判断与最小处理 |
|---|---|---|
| 必须处理 | `src/official_spill.hpp:5` 明示 Port of Scene B / Step1 / Step2；`src/official_timing.cpp:158` 实现逐核 Step3，`:402–416` 执行逐核和全局官方计时；`README.md:3,20` 明示官方计时移植 | §2.4 禁止官方评估器副本。它不是逐字复制 Python，但属于刻意复现评估器实现、顺序与数值行为的移植，严格提交不应将其当作普通自主启发式。移植文件和依赖回放须从提交源码及链接中一起排除。不能泛化为“所有自主估价模型都被禁止”。 |
| 必须处理 | `CMakeLists.txt:10` 正式链接 `official_spill.cpp`、`official_timing.cpp`、`spill_feedback.cpp`、`v10_search.cpp`；`src/main.cpp:158–173` 正式调用回放与精算 | 不是仅测试存在。`v10_search.cpp:91,131,194,205,233` 实际用官方轨迹/计时筛选候选。不能原样引入整个 v10 主线。 |
| 来源缺口 | `src/official_timing.cpp:360–399` 明示 CPython 3.11.11 `Objects/setobject.c` 和 `longobject.c` 的函数/哈希规则 | v10 路径文件清单及源包清单无 `third_party/`、LICENSE，README 无第三方库清单。若保留此派生实现，须厘清来源并依规则提供版本、许可证和源码路径；当前最小合规路线是随精算模块一起剥离。本审计不作具体版权法律结论。 |
| 必须修复 | `src/main.cpp:95–98` 用固定 `<output>.tmp` 并 `ios::trunc` | 输入若恰为该临时文件，虽然输入/输出不同，生成后仍会截断输入并将其重命名；已有无关临时文件也会被覆盖。采用 0.8.3 已修复的独占临时目录实现，不回退为 v10 入口。 |
| 必须修复 | `src/main.cpp:176–184` 捕获全部求解异常返回 5，序列化与写文件统一返回 4；`src/chain.cpp:118` 的 NoCandidate 仍在匿名作用域 | §4.2 区分内部错误 1、无合法方案 5、输出错误 4。复用 0.8.3 的公共 NoCandidate、内部异常归 1、序列化与保存分离；不要把 bad_alloc/内部缺陷伪装为无解。 |
| 交付调整 | `scripts/package.py:7,14–18` 默认根名不是 17、glob 全部 src；`reports/upgrade/PACKAGE_V10_1.json` 记载根 `dag_chain_solver_v10_1/`，包括全部官方移植文件 | 已有源包只有约 116 KB，未混入 build/data，但不是可直接交付的 `17.zip`。最终需白名单打包，根目录 17，并排除评估器及研究依赖。 |
| 文档/诊断 | README 未列完整 Windows 构建命令或第三方清单；`main.cpp:161,167,172` 输出反馈及 exact cycles 诊断 | README 补齐 §2.3。§1.2 规定程序只输出方案、不自行输出评分结果；严格提交入口应去掉精算评分诊断，方案 JSON 本身只有规定两字段。 |

固定提交来源：[v10 提交](https://github.com/xztqaqmax67-cmyk/2626_hackson_17/commit/b709f70305e924a4c3f3a359264262d1b8b9e525)、[main.cpp](https://github.com/xztqaqmax67-cmyk/2626_hackson_17/blob/b709f70305e924a4c3f3a359264262d1b8b9e525/dag_chain_solver_v10/src/main.cpp)、[official_timing.cpp](https://github.com/xztqaqmax67-cmyk/2626_hackson_17/blob/b709f70305e924a4c3f3a359264262d1b8b9e525/dag_chain_solver_v10/src/official_timing.cpp)。

## 已符合或未发现问题的部分

- `src/model.hpp:332–340` 固定 L1=1572864、UB=131072、DDR=60、跨核=1000；全部生产源码未发现 getenv/config.txt 或其他覆盖入口。
- 正式 CLI 支持 2..5/default4、长选项的空格与等号形式、默认输出、未知参数退出2。`--ablation` 只在开发目标的 CHAIN_EXPERIMENT 宏下存在，不属于正式入口。
- CMake 3.16/C++17、目标 npu_sched、单/多配置 bin 路径、相对源码路径、默认关闭测试均符合结构要求；平台 API 有 `_WIN32` 隔离。没有发现生产代码访问网络、外部进程、历史答案或公开用例名特判。
- 默认 `enable_timed_search=false`，候选配额固定；`parallel_jobs.hpp:26–56` 最多4个工作，独立槽位、固定下标归并，异常优先级固定。v10 新阶段的 Clock 只记录诊断时间，不参与默认候选选择。源码层面未发现随机设备或墙钟截断影响默认输出。
- v10 摘要记录：两编译器各17项 CTest、64组合成图重复一致、exact_pool/exact 共80组官评通过。**这是对方保留的历史证据，本次没有复跑，也不能把80次两个阶段官评当成正式版本40组逐字节重复验证。**

## 运行时限与跨平台边界

v10 README 报告默认 exact 平均9.982秒、最大29.273秒，远低于90秒，但无任意隐藏输入的硬时限证明。固定工作量并非违规；它避免墙钟导致输出不一致，但仍须在最终源码/编译环境下实测90秒、16GB和10线程门槛。其全阶段会叠加原搜索、回放、分组及最多14次完整多核精算，不能把 `total_seconds=15` 当整体截止。

验证摘要明确只测 Windows MSVC/MinGW；原生 Linux 和较旧 GCC 尚未测试。CPython 3.11.11 整数 set 顺序被硬复现，这不等于按用例硬编码，但使“精确等价”依赖官方运行时版本；评估器环境变化仍可能改变排序与周期。

## 后续可独立吸收的最小路线（本次未实施）

1. 保留当前 0.8.2/0.8.3 的固定工作量入口、消费者搜索、CLI、安全写出与自主两级估价，避免因整体换主线丢失已验证收益和修复。
2. 可拆出 `tensor_groups.cpp:9–74,99–197` 的 Tensor 聚合、消费者聚合、完整 writer 原子拆分、存活区间压力和三方向交替枚举。其官方 trace 仅在 `:77–98` 为热度加权；应删去官方类型依赖，以自主压力统计替代，或先省略该权重。不要直接 include 原 header。
3. 以现有 evaluate_proxy/refine_estimate 固定配额独立评价候选，保留入口解；官方评分只在包外验证。原 `v10_search` 的收益不能直接外推为去掉精算后的收益。
4. `communication.cpp` 前段分组生成也可另拆；其 improve 包装依赖 official_spill，不宜原样接入。时间线尾段依赖 OfficialTimingResult，需重新设计自主诊断后才能采用，不适合快速收尾。
5. 每个真正融合候选须单独版本化并完成同口径40组、两次字节一致、资源与合成/CLI/跨平台验收。当前用户要求尽快收尾，本次不再引入未验证算法。

## 对当前 0.8.3 最终暂存工程的复查

只读检查 `overnight/final_stage/17`：当前为 CMakeLists、README 和20个自主源码/头文件；未见官方评估器模块、第三方库、测试数据或构建产物。CMake 目标只链接8个自主实现 .cpp，版本0.8.3。main 直接调用 solve_fixed_work，正确区分 NoCandidate/内部/输入/输出错误，序列化先于保存，独占临时目录避免覆盖既有路径；未输出评分。

主线程仍在整理排版，因此此处按函数而非易变化行号定位：`main.cpp::TemporaryOutput/run`、`anytime.cpp::solve_fixed_work`、`model.hpp::Config/solution_json`。审计时发现 README 标题与 --version 仍写0.8.2，已通知主线程同步。最终40组、源包逐字节比较及Linux/Windows CI由主线程回填，不能用此静态审查代替运行验收。
