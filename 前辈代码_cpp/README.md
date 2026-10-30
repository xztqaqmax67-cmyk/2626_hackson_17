# 前辈代码的 C++ 版本（Hackathon 场景 B）

## 2026-10-30 激进搜索优化合并

已吸收 `perf_aggressive.rar` 的按需 profile、提前重块拆分/四段细分、短预算重点搜索和种子流水线下界过滤。结构/流水线种子采用可停止的访问接口，在现有线程池内只构造 compact 搜索可使用的前两个候选；局部构造超时仍保留已完成候选。保留当前链初解、`new90`、官方规则和并行实现。

早期细分同时用于正式与本地 compact 搜索；仅在搜索预算不超过 30 秒时跳过旧构造器，并在早期 Makespan 改善超过 2% 后收缩广泛分核试探。正式 `npu_sched` 仍为 90 秒，本次按用户要求只用 `npu_sched_local` 的 **30 秒总预算、25 秒搜索截止**做新旧配对。构建位置为 `build/aggressive_merge_20261030/bin/`，对照记录见 [合并与 30 秒测试](reports/AGGRESSIVE_MERGE_20261030.md)。

## 2026-10-30 并行优化与本地时限

当前搜索共用一个最多三条后台线程的固定计算池，调用线程也参加计算。独立种子来源、代表种子的重排构造、候选 `new90` 粗筛、候选小批量精评，以及精评中的每核准备均可并行；嵌套调用就地顺序执行，不额外启动线程。全局 DDR/COPY 事件模拟、路线选择、缓存提交和最好解更新仍顺序执行。候选结果按输入顺序接收，线程完成顺序不决定平局选择；限时搜索和按耗时分配路线仍可能改变最终搜索路径。

种子阶段已移除休眠生成器线程。搜索阶段保留最多四条 compact 路线线程（full 最多六条）；加主线程和三条计算池线程，算法自身最多创建 8 / 10 条线程。运行库线程、实际进程峰值和内存尚未测量。每次邻域预评最多三个候选；固定评估次数模式关闭预评，保留次数上限。实现同时减少了候选分组复制、profile JSON 复制和缓存查找成本。

| 程序 | 默认总时限 | 用途 |
|---|---:|---|
| `npu_sched` | 90 秒，预留 5 秒收尾 | 正式评测；默认构建目标 |
| `npu_sched_local` | 30 秒，预留 5 秒收尾 | 本地快速运行；显式构建目标 |
| `problem2` | 单图默认 30 秒，可用 `--time-limit` 指定 | 本地开发；批量模式仍使用共享总预算 |

本地入口与正式入口使用相同算法库。`NPU_LOCAL_RUN` 仅在 `npu_sched_local` 目标上定义，不通过运行参数或环境变量改变正式程序的 90 秒预算。

```powershell
cmake --build build/parallel_20261030 --config Release --target npu_sched npu_sched_local
# 后续需要快速运行时使用这个本地程序；本次未运行：
.\build\parallel_20261030\bin\npu_sched_local.exe graph.json -n 4 -o plan.json
```

最初按用户要求仅做源码检查和编译；后续获得测速授权，已完成四核 10 例、每次 30 秒预算的串行/并行对照。官方 Python 完整复评 20/20 一致，本轮方案质量 4 胜 6 平；固定方案评分约快 1.88 倍。并行搜索观察最多 9 条线程、约 1.67 GiB RSS，均在 30 秒内结束。这是本机四核单轮搜索观察，不代表 2..5 核总分、90 秒正式运行或任意输入的资源上限。详见 [30 秒官方复评与测速](reports/PARALLEL_BENCHMARK_20261030.md)；实现细节见 [并行修改记录](reports/PARALLEL_20261030.md)。

## 当前源码与构建

当前版本已移除 `tests/`、CMake 测试目标及 Python 测试入口。默认构建正式程序 `npu_sched`；本地快速运行、开发求解、独立评分与示例方案工具仍可显式构建。本次并行版本的构建目录为 `build/parallel_20261030/bin/`。删除测试时曾通过示例求解和独立评分，下文这些数字属于本次并行修改前的历史记录。

## 当前唯一估价：new90

候选筛选统一使用新标准的90%阈值换页估价，已删除旧估价、半权重、计算余量、COPY事件版本及模式切换接口。正式程序和开发求解入口均自动生效，最终方案仍由完整评分决定。

估价为 F = max(T, (R+S)/B) + S/B + 0.03(R+S)/B：T为流水线草图，R为路由字节数，S为按软阈值、有效备份和脏回写计算的换页字节数。稠密访问索引每次求解只构建一次；已移除未采用版本的事件、流水线负载和部分写者提示计算。

选择依据：此前10个四核候选池测试取得5胜5平；两例固定精评预算的完整搜索最终打平，因此不承诺完整搜索提速。历史比较见[估价变体报告](reports/PROXY_VARIANTS_20261030.md)，本次采用说明见[单估价采用说明](reports/NEW90_ADOPTION_20261030.md)。

## 2026-10-30 性能优化合并

已合入 `perf_optimization.rar` 的 JSON/索引优化、单遍跨度汇总、标量接口去明细、排序评分预计算、只读结果共享/缓存和 Pipe 定长数组，保留当前链初解与新版官方评分规则。9 组回归通过；10 个固定四核方案完整评分结果一致，本机重复计时的合计评分耗时约减少 39%（1.64×）。这是固定工作量评分的计时，不能视为整体搜索或比赛分数提升。

合并范围、逐例计时、验证和复现命令见 [性能合并记录](reports/perf_merge_20261030.md)。本次独立构建的正式程序位于 `build/perf_merge/bin/npu_sched.exe`。


## 2026-10-30 官方评测对齐

用户提供的 `hackathon.zip` 已原样导入 [`official_evaluator/`](official_evaluator/README.md)，包含官方代码、10 个用例和配置；来源哈希见 `official_evaluator/SOURCE_MANIFEST.json`。

当前 C++ 完整评分已同步官方 **90% 提前换出、多写者重命名/脏回写、扩展序列拓扑检查和写者同核校验**。`npu_sched` 与 `problem2` 共用这些修正。测试与外部评测直接以此份官方 Python 为准，已移除旧参考的 PIPE/COPY 补丁；历史链初解评测使用官方 Python，并按官方源码哈希隔离旧缓存。

验证：8/8 组回归通过；已有批量方案及最新链初解各 10 个四核方案，共 **20/20 的内部完整结果与官方一致**。每批有 7 例的旧评分发生变化，详细分差、原因和复现命令见 [官方对齐报告](reports/official_alignment_20261030.md)。下文及旧报告中早先的旧版对照成绩属于历史口径。



## 当前原地改造方式

本目录集中维护**问题 2／场景 B**。已移除问题 1、问题 3、独立单核评估器及对应构建目标、配置和测试分支；问题 2 评估器中的共享缓存分支也已移除。正式入口和开发求解入口均只接受 2..5 核。

多核搜索中的 `single_core_plan(graph, cores)` 仍用于构造初始候选：方案有 `cores` 个核，暂时将工作集中到一个核，随后尝试迁移和激活空闲核。这属于场景 B 搜索流程。

初次阅读请看 [代码阅读指南](https://github.com/xztqaqmax67-cmyk/2626_hackson_17/blob/main/%E5%89%8D%E8%BE%88%E4%BB%A3%E7%A0%81_cpp/%E4%BB%A3%E7%A0%81%E9%98%85%E8%AF%BB%E6%8C%87%E5%8D%97.md)：包含整体调用关系、结构体字段、各模块函数与局部辅助函数说明，以及从入口追踪到评分的示例。

本次清理已通过 Windows / MinGW 编译，以及五组回归测试：生成器、场景 B 完整评分对照、搜索对照、命令行工作流和硬件/批量约束。搜索对照首次因 Python 参考程序替换临时文件时拒绝访问而中断，重跑通过。

默认构建 `npu_sched`，直接调用原 `search.cpp` 的同一套完整搜索：链剖分初解、结构种子、流水线候选、精英池、多路线、切块合并、交换迁移、生命周期修复、空闲核激活和完整评分均保留。没有使用精简版的内部估价替换原算法。

2026-10-30 新增 `problem2/chain.cpp`：在原初解阶段加入最多两个链候选，再进入原有完整评分和后续搜索。正常使用无需新增参数；测试期间的“只用链初解”运行模式已移除。链段仅约束分核，输出仍保留每个算子的原始依赖，避免缩点引入调度环。实现、论文依据、四核 10 例对照及参考解说明见 [链初解评测记录](reports/chain_initial_20261030.md)。

正式入口使用固定参数，只输出两字段方案。原 `problem2` 开发入口仍支持配置读取、检查点继承、批量运行和评分导出。两者共用 `solve_impl`；正式入口由内存传入图和参数、返回最好方案，不读写中间文件、不打印评分、不创建子进程。

Linux（GCC 9.4/11，CMake 3.16+），在本目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/npu_sched "/path with spaces/graph.json" -n 4 -o "out/plan.json"
```

Windows（VS2019 16.8+ / VS2022），在本目录执行：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release -j
.\build\bin\npu_sched.exe "C:\path with spaces\graph.json" --cores=4 --output="out\plan.json"
```

多配置生成器也直接输出 `build/bin/npu_sched.exe`。换编译器时使用新的构建目录。

`npu_sched` 的核数范围为 2..5、默认 4，支持 `--version`、`--help`、两种参数写法。省略输出路径时写到图同目录的 `<case>_multicore_res.json`。不接受配置、检查点或评分导出参数。退出码：0 成功、1 内部错误、2 参数错误、3 输入错误、4 输出错误、5 无合法方案。

正式验收以 `材料/提交要求文档.md` v3.0 为准；与旧 `备注.txt` 冲突时采用提交要求。`npu_sched` 按单次运行 **90 秒**分配预算：从程序开始计时，搜索截止于第 **85 秒**，实际搜索额度扣除输入处理耗时，最后 **5 秒**用于校验和写出。正式入口不受 `DEFAULT_CASE_TIME_LIMIT = 30` 限制，也不受开发批量入口的共享预算限制；该常量仅是 `problem2` 开发入口的单图默认值。候选搜索提前结束时可提前输出。

保留 compact 探索宽度，路线交替推进，路线内部的独立工作在固定线程池执行。工作线程显式继承截止时间，局部构造时限服从总时限。限时搜索仍不保证重复输出完全相同；90 秒、16 GB 及指定 GCC/MSVC 环境的完整验收仍待核验。截止时间通过协作式检查实现，5 秒收尾预留仍须结合大图与评测机器验证，不能视为硬实时保证。

本次时限调整已在当前 Windows / MinGW 环境编译并验证（4 核）：`example_graph.json` 整个进程耗时 0.13 秒，`case_03.json` 耗时 86.86 秒，均退出 0、只输出规定的两字段方案，且通过独立 C++ 场景 B 本地复评。大例子确认正式入口已不受 25 秒预算限制；这两例不代替全用例和指定平台的正式验收。

**留待最后调整：** 源码、第三方库仍在原 `problem2/` 目录下；场景 B 评估器移植、配置和开发工具仍保留。本目录尚未完成最终打包，不能直接宣称满足“不得包含官方评估器副本”等条款。

自有 C++ 代码统一为四空格缩进，Tab 显示宽度 4，禁止实际 Tab 字符；`.clang-format` 与 `.editorconfig` 一起约束后续编辑。公共结构、搜索路线和接口边界补充中文注释，第三方文件保留上游格式。

此前接口适配已检查新旧入口在 1、6、24 次固定评估预算下输出相同方案，并验证正式入口参数、退出码、中文/空格路径及无配置运行。本次模块清理继续保留多核搜索的邻域、种子、路线、修复和选择逻辑。

以下是原模块说明和开发工具用法。运行原工具前，显式构建目标：

```powershell
cmake --build build --config Release --target problem2 multicore_cut_evaluate_problem_2
```

保留问题 2 所需的原模块名称和 `problem2/official_code/` 层次，函数声明放在 `.hpp`，实现放在同名 `.cpp`；包入口 `__init__.py` 对应 `__init__.hpp`。同级原 Python 源码和题目数据没有修改。

算法使用原生 **C++17**，以标准 CMake 工程支持 **Windows / Linux**，运行时不调用 Python。关键步骤、数据结构和易混淆的执行规则有中文注释。

## 当前提交约束与开发兼容性

正式入口的硬件参数以 `材料/提交要求文档.md` 第 5.2 节为准，和原数学建模附件的默认配置有所不同：

| 参数 | 当前值 |
|---|---|
| L1 容量 | 1.5 MiB = **1,572,864 bytes** |
| UB 容量 | 128 KiB = **131,072 bytes** |
| DDR 带宽 | **60 bytes/cycle** |
| 场景 B 跨核 COPY 等待 | **1000 cycles** |

提交要求还规定：C++17 及以下、Windows / Linux 标准 CMake 构建、无复杂第三方依赖，运行线程数不超过 10，内存不超过 16 GB。求解程序按顺序运行用例和搜索路线；`-n` 指模拟芯片核数，不控制主机并行度。当前 C++17 版未重新测量整批用例的内存峰值。

`PIPE_FIX` 是第五条独立流水线，与 `PIPE_MTE2`、`PIPE_MTE3`、`PIPE_M`、`PIPE_V` 并列，每条流水线仍只有一个在飞指令。`L1_COPY_UB`、`L1_LOOP_ENHANCE` 等操作按输入的 `pipe` 和 `cycles` 执行，不被误当作 DDR COPY。

第 2 步沿用最晚使用 victim 规则，并已按新官方口径加入 90% 软阈值和多写者换出修复；第 3 步沿用乱序/FIFO/内存复用规则。核内顺序优化仍通过标准方案表达：`pipeline.cpp` 的候选将每个非 DDR COPY 算子作为单独子图，再给出每核子图顺序。搜索也保留较粗分块候选，只有完整评估更好的方案才会保留。目标按 `(makespan, added_copy_bytes)` 比较，优先执行时间，其次数据搬运量。

## 开发工具：共享 5 分钟的批量运行

本节是保留的 `problem2` 开发工具用法；300 秒只是该批量接口的默认预算，不作为正式 `npu_sched` 的验收时限。

在本目录执行（以下以 **4 核**为例，核数仍由 `-n` 指定）：

```powershell
.\build\bin\problem2.exe ..\hackathon_data -n 4 --config problem2/config.txt --output-dir output/hackathon_cores_4 --work-dir build/batch_work --total-time-limit 300
```

第一个参数传入目录时启用原生 C++ 批量入口，读取该目录直接包含的所有 `.json` 图。默认整批 300 秒，在尚未运行的用例间均分剩余时间；某例提前结束，其余时间自动让给后续用例。单例额度的 25%（最多 10 秒）留给检查点读取、校验和 JSON 写出，整批另留最多 3 秒汇总；批量模式不允许额外首解延时。算法没有按 `case_XX`、图指纹或测试集内容作特化。

每个用例输出 `<图名>_multicore_res.json`，仍只有题目规定的两个字段。`batch_report.json` 记录每例是否成功、makespan、搬运量和整批耗时。预算不足或某例失败时返回非零状态并明确标记，不会把旧文件当成新结果。`--work-dir` 必须为空；再次运行请换一个新工作目录，或省略此选项使用临时目录。

这是共享时间上限，不保证任意机器、任意大图都能在上限内找到有效解；是否全部产出以报告中的 `completed` 为准。`--time-limit` 在批量模式下还可设置单例上限，但不会增加整批 300 秒的上限。

**此前 C++20 版本的实测（2026-10-30，当前 Windows 电脑，`-n 4`）：10/10 个原始用例全部产出，完整进程墙钟耗时 282.20 秒，约 4 分 42 秒。** 本地结果在 `output/hackathon_cores_4/`；`batch_report.json` 是逐例报告，`wall_clock.json` 是外部计时，`verification_report.json` 是独立复核记录。这些生成文件不提交到 Git。当前 C++17 版已通过编译和下述回归测试，尚未重跑这轮 10 例计时，不能直接把旧耗时当作新版性能结果。

## 先运行一个例子

在本目录打开 PowerShell。当前电脑已经编译好 `build/bin/` 下的程序，可直接运行：

```powershell
.\build\bin\problem2.exe examples/example_graph.json -n 3 --config problem2/config.txt --time-limit 10 --output-dir output --export-evaluation
```

输出文件：

- `output/example_graph_multicore_res.json`：标准方案，仅含 `node_to_subgraph` 和 `core_schedules`。
- `output/example_graph_problem_2_res.json`：完整评分结果。
- `output/example_graph_problem_2_trace.json`：Perfetto / Chrome 时间线。
- `output/example_graph_problem_2_log.txt`：评估日志。

换成实际计算图时，替换第一个路径即可。省略 `--config` 会读取计算图同目录的 `config.txt`；批量模式读取输入目录中的配置。开发工具建议明确传入与提交要求硬件参数一致的 `problem2/config.txt`；正式入口不读取此文件。

**下发图现已可直接读取：** 允许 `COPY_IN` / `COPY_OUT` 的 `cycles=-1` 作为带宽计算占位符，实际 COPY 耗时仍按原有规则由张量大小和带宽决定。其他负耗时、未知 Pipe 仍会报错。`PIPE_FIX` 已在输入校验、搜索估算、独立评估和时间线中统一接入。整个流程不改写磁盘上的原始图。

## 重新编译

需要支持 C++17 的编译器、CMake 3.16+ 和构建工具。工程固定使用 C++17，关闭语言扩展；GCC / Clang 还启用 `-pedantic-errors`，避免意外依赖更新标准。第三方头文件已放在 `problem2/third_party/`，正常编译不需要联网、Python 或额外库安装。

以下命令均在本目录执行。Windows 使用 MinGW-w64 与 Ninja：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
```

Linux 使用 GCC / Clang 与 Make / Ninja（编译器需提供 C++17 的 `std::filesystem`，建议 GCC 9+）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/bin/problem2 examples/example_graph.json -n 3 --config problem2/config.txt --time-limit 10 --output-dir output
```

两个系统各自建立 `build`，不要把另一个系统生成的构建缓存复制过来。Linux 可执行文件没有 `.exe` 后缀；源文件和输入输出 JSON 共用。Windows 的中文参数、进程创建使用宽字符 API，Linux 使用 UTF-8 路径和 POSIX 进程管理，两者都处理从 `PATH` 启动的情况。

Windows 也可使用安装了 C++ 桌面开发工具的 Visual Studio：

```powershell
cmake -S . -B build/msvc -A x64
cmake --build build/msvc --config Release --parallel 4 --target problem2
.\build\msvc\bin\problem2.exe --help
```

MinGW 构建会静态链接 C++ 运行库，便于复制可执行文件运行。标准库线程由编译器运行库和系统 pthread 提供，不依赖 C++20 协程或第三方协程库。`cmake/zig-linux.cmake` 是可选的 Windows → Linux x86-64 交叉编译工具链；正常在 Linux 上构建无需使用它。

**验证范围：** Windows x64 使用 GCC 16.1.0 实际编译、运行回归测试；Linux x86-64 使用 Zig 0.13.0 自带的 Clang / libc++ 交叉编译。当前电脑没有 Linux 发行版，Linux 运行验证和 MSVC 编译验证尚未执行。

正式入口及保留的三个开发工具位于 `build/bin/`（默认只构建 `npu_sched`，开发工具须显式构建）：

| 程序 | 用途 |
|---|---|
| `npu_sched.exe` | 正式提交入口，固定硬件和 90 秒总预算 |
| `problem2.exe` | 问题 2 开发求解入口，支持调试、继承和批量运行 |
| `multicore_cut_evaluate_problem_2.exe` | 问题 2 / 场景 B 独立评估 |
| `stub_multicore_cut_and_schedule.exe` | 生成随机示例方案 |

不同平台或多配置构建器的可执行文件后缀、配置子目录可能不同。

## 原文件与 C++ 文件的对应关系

下面的模块除 `main` 和 `__init__` 外，都保留同名 `.cpp` 与 `.hpp`。`main` 对应 `main.cpp`，`__init__` 对应 `__init__.hpp`。

| 原模块（省略 `.py`） | 主要内容 |
|---|---|
| `problem2/__init__` | 公共头文件入口 |
| `problem2/main` | 参数、输入输出、搜索进程及时间上限 |
| `problem2/core` | 图索引、方案、分块变换、搬运与生命周期估算 |
| `problem2/bootstrap` | 快速构造首解 |
| `problem2/structure` | 图层次、分支、分隔与结构种子 |
| `problem2/pipeline` | 流水线就绪模型与顺序优化 |
| `problem2/activation` | 尝试启用空闲核心 |
| `problem2/portfolio` | 多路线、精英方案和探索机会分配 |
| `problem2/search` | 候选生成、正式评估、保优与终止 |
| `problem2/official` | 评估包装、边界图和执行分析 |
| `problem2/inheritance` | 检查点指纹、方案继承和重新验证 |
| `problem2/artifacts` | 方案、评估和时间线导出 |
| `problem2/submission_io` | 用户可见错误信息处理 |
| `problem2/time_budget` | 子进程、限时和首次有效解延时 |
| `problem2/utils` | JSON、文本、原子写入和 SHA-256 |
| `problem2/official_code/contest_io` | 官方入口的公共 I/O 和日志格式 |
| `problem2/official_code/evaluation_validation` | 输入、参数、依赖与执行顺序校验 |
| `problem2/official_code/schedule_step1` | 固定调度序列 |
| `problem2/official_code/schedule_step2` | 内存换出、换入和张量重载 |
| `problem2/official_code/schedule_step3` | FIFO、内存复用依赖和执行时间线 |
| `problem2/official_code/multicore_cut_evaluate_problem_2` | 场景 B |
| `problem2/official_code/stub_multicore_cut_and_schedule` | 随机示例方案及方案派生信息 |

额外的 `common.hpp` 集中公共类型与五条 Pipe 的硬件表；`batch.cpp/.hpp` 管理整批时间预算；`python_order.hpp` 复现影响内存复用的 Python 整数集合顺序；`generator.hpp` 用 C++17 标准线程和条件变量保存多路线生成器的推进位置；`utf8_entry.hpp` 处理 Windows 中文参数及两系统的程序路径；`official_code/cli_main.cpp` 为各评估程序提供薄入口，算法仍在各自原名文件中。

生成器每次只唤醒一条路线，其余路线休眠；被唤醒路线的独立粗筛、精评和每核准备使用 `parallel.hpp` 的计算池。取消生成器时先通知退出并等待线程结束，再释放外部数据；池中工作也必须全部完成后才退出该批次。

建议阅读顺序：`main.cpp` → `core.hpp` → `search.cpp` → `official.cpp` → `official_code/schedule_step1/2/3.cpp`。图中的 **op** 是算子，**tensor** 是张量，**block/subgraph** 是子图块，**core** 是核心，**pipe** 是流水线。场景 B 中同核全部子图合为一个 **Task**。

## 开发工具的常用参数与复评

以下为 `problem2` 的参数；正式入口的 90 秒预算由程序内固定常量控制。

- `-n 2..5`：**待模拟芯片的核心数**，不代表求解程序使用的电脑 CPU 核数。
- `--time-limit SECONDS` / `--budget SECONDS`：单例时限，范围 `(1, 1200]`。单图默认 **30 秒**；目录模式默认动态均分余时，显式设置则作为单例上限。
- `--total-time-limit SECONDS`：仅目录模式，范围 `(1, 300]`，默认 **300 秒**。
- `--feasibility-grace SECONDS`：单图调试时可显式启用首解延时，默认 **0 秒**，最多 120 秒；批量模式必须为 0，避免突破总预算。
- `--search-profile compact|full`：默认 `compact`，`full` 搜索更宽。
- `-o FILE` / `--output-dir DIR`：方案文件或输出目录，两者互斥。
- `--export-evaluation`：同时导出评估结果、日志和时间线。
- `--work-dir DIR`：保留 `best_checkpoint.json`、`metrics.json` 和搜索日志；目录须为空、位于源码目录外，并与最终输出分开。不指定时使用自动清理的临时目录。
- `--inherit-checkpoint FILE`：从较低核数继承已验证方案，在目标核数重新评分。

独立复评刚才的方案：

```powershell
.\build\bin\multicore_cut_evaluate_problem_2.exe examples/example_graph.json output/example_graph_multicore_res.json --config problem2/config.txt -o output/check_res.json --trace-output output/check_trace.json --log-output output/check_log.txt
```

各保留入口支持 `--help`。

检查点会校验输入和配置的 SHA-256。C++ 评估模块及公共硬件表的源码指纹在编译时写入程序，移动程序不影响继承。也接受来源与 `original_official_hashes.json` 一致、且输入和配置哈希均一致的 Python 检查点，但必须重新评估；旧容量/等待参数的检查点不能冒充当前参数的结果。修改评估模块或硬件表后须重新编译，旧 C++ 检查点会因指纹不符而拒绝。

`original_official_hashes.json` 保留原 Python 附件的完整历史指纹，因此仍有已移除模块的文件名。这只是旧检查点的来源识别数据，不包含或编译相应算法。

## 已执行的对照验证

对比完整结果结构中的数组顺序、时间线、评分、搬运量和峰值，不只检查最终 makespan。移除测试前的回归验证覆盖：

- 20 组小型 DAG，其中 4 组包含 `PIPE_FIX`：跨核搬运、直接依赖、执行分析、三类邻域与核心启用候选一致。
- 6 组固定评估次数搜索：剔除耗时字段后，候选历史、统计项、最终评分和最优方案一致。
- 2 组更长的搜索：确认进入路线探索，并用原 Python 评估器独立复核完整评分。路线分配使用实测耗时，因此此阶段不要求跨语言的候选出现顺序一致。
- C++17 生成器的延迟启动、暂停/恢复、移动、取消清理、异常传播和截止时间传递。
- 中文路径、场景 B 开发入口、检查点继承、2 秒限时、拒绝单核求解、错误输入不覆盖旧结果。
- 独立 `PIPE_FIX` 与 M/V 重叠执行、同一 Pipe 串行、1 MiB 的 L1 张量、COPY `-1` 占位、非法负耗时和未知 Pipe 拒绝。
- 原生批量入口处理任意图名、共享时限、输出字段检查、失败不误用残留方案。
- 此前 C++20 版的真实 10 例批量输出：逐个使用补齐硬件表的 Python 场景 B 原算法重新评分，与 C++ 检查点的**完整结果结构**全部一致；C++17 版未重跑整批。

**对照范围：** 旧 Python 附件本身没有第五条 Pipe，也拒绝 COPY `-1`。测试仅在内存中给原版补充提交要求中的 `PIPE_FIX` 硬件列表，并为参考输入替换 COPY 占位值，调度函数保持原样；C++ 正式入口直接读取原始图。这是旧算法的本地对照，不代表已经拿到并验证过新比赛官方评估器。原 Python 文件及下发数据均未改动。

测试源码及运行脚本已按要求移除，历史结果保留在 `reports/` 中。

## 等价范围与差异

这是保持原计算规则和文件结构的移植，以上对照已通过；测试不能证明任意输入都逐位一致。

- 限时搜索取决于实际耗时，C++ 和 Python 在相同秒数内可能探索不同数量的方案，因此最终最优方案可能不同。耗时、进程返回码等运行记录也属于平台相关信息。
- ID、字节数、计数采用有符号 64 位整数，输入及中间整数运算须在其范围内；不支持 Python 的任意精度整数语义。时间计算使用 `double`。
- 保留标准方案与评估结果字段；JSON 缩进、数值文本形式、报错文案及异常类型不要求逐字相同。内部 Python 元组键等用明确的 C++ 类型或 JSON 数组表示。
- 依赖顺序参考 64 位 CPython 整数集合规则，并已与当前原版实际执行结果对照；不能据此保证其他 Python 实现的集合遍历顺序。

## 第三方代码

- nlohmann/json 3.11.3：有序 JSON 读写，MIT 许可证见 `problem2/third_party/LICENSE.json`。
- PicoSHA2 1.0.1：SHA-256，MIT 许可证保留在 `problem2/third_party/picosha2.h` 文件头。

这些文件与本项目源码一同保留，构建无在线下载步骤。
