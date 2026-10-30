# 前辈代码的 C++ 版本（Hackathon 场景 B）

## 当前原地改造方式

本目录是当前继续维护的完整移植工程。按用户最新要求，在原文件内适配接口，**保留全部算法、模块和文件**。`17/` 与 `17.zip` 是之前的精简草稿，暂时保留，不作为本次成果；最终目录整理、文件剔除及打包，等待用户最后明确要求后再做。

默认构建 `npu_sched`，直接调用原 `search.cpp` 的同一套完整搜索：结构种子、流水线候选、精英池、多路线、切块合并、交换迁移、生命周期修复、空闲核激活和完整评分均保留。没有使用精简版的内部估价替换原算法。

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

多配置生成器也直接输出 `build/bin/npu_sched.exe`。换编译器时使用新的构建目录；旧缓存曾开启测试的，可显式传入 `-DBUILD_TESTING=OFF`。

`npu_sched` 的核数范围为 2..5、默认 4，支持 `--version`、`--help`、两种参数写法。省略输出路径时写到图同目录的 `<case>_multicore_res.json`。不接受配置、检查点或评分导出参数。退出码：0 成功、1 内部错误、2 参数错误、3 输入错误、4 输出错误、5 无合法方案。

仍保留默认 25 秒限时搜索及 compact 探索宽度。生成器最多使用 6 条休眠路线线程加 1 条主线程，compact 最多 4 条路线，路线交替运行。按用户要求保留限时搜索，因此不保证重复输出完全相同；90 秒、16 GB 及指定 GCC/MSVC 环境的完整验收仍待核验。

**留待最后调整：** 源码、第三方库仍在原 `problem2/` 目录下；评估器移植、配置、测试和开发工具全部保留。本目录尚未完成最终打包，不能直接宣称满足“不得包含官方评估器副本”等条款。

自有 C++ 代码统一为四空格缩进，Tab 显示宽度 4，禁止实际 Tab 字符；`.clang-format` 与 `.editorconfig` 一起约束后续编辑。公共结构、搜索路线和接口边界补充中文注释，第三方文件保留上游格式。

本轮检查：保留原工程全部 74 个已跟踪文件；49 个未涉及接口适配的算法文件，忽略空白与注释后代码一致。搜索的邻域、种子、路线、修复和选择逻辑均未裁剪；步骤 2、3 原文未改。新旧入口在 1、6、24 次固定评估预算下输出相同方案。正式入口的参数、退出码、中文/空格路径及无配置运行检查通过。完整工程通过 Windows MinGW 编译和 Linux 交叉编译；未重跑整批性能测试，未在 MSVC 或原生 Linux GCC 环境验收。

以下是原模块说明和开发工具用法。运行原工具前，显式构建目标：

```powershell
cmake --build build --config Release --target problem2 multicore_cut_evaluate_problem_2
```

原目录内的 **25 个 Python 文件**均已对应移植，包括 `official_code` 中的问题 1、2、3 评估器。保留 `problem2/official_code/` 层次和原模块名称，函数声明放在 `.hpp`，实现放在同名 `.cpp`；包入口 `__init__.py` 对应 `__init__.hpp`。原 Python 源码和题目数据没有修改。

算法使用原生 **C++17**，以标准 CMake 工程支持 **Windows / Linux**，运行时不调用 Python。关键步骤、数据结构和易混淆的执行规则有中文注释。`tests/` 中的 Python 只负责调用原版进行对照，不是运行依赖。

## 根据备注更新的比赛约束

本版已按 `材料/备注.txt` 更新硬件参数，和原数学建模附件的默认配置有所不同：

| 参数 | 当前值 |
|---|---|
| L1 容量 | 1.5 MiB = **1,572,864 bytes** |
| UB 容量 | 128 KiB = **131,072 bytes** |
| DDR 带宽 | **60 bytes/cycle** |
| 场景 B 跨核 COPY 等待 | **1000 cycles** |

新备注还要求：C++17 及以下、Windows / Linux 标准 CMake 构建、无复杂第三方依赖，运行资源不超过 10 个 CPU 核和 16 GB 内存。求解程序按顺序运行用例和搜索路线；`-n` 指模拟芯片核数，不控制主机并行度。当前 C++17 版未重新测量整批用例的内存峰值。

`PIPE_FIX` 是第五条独立流水线，与 `PIPE_MTE2`、`PIPE_MTE3`、`PIPE_M`、`PIPE_V` 并列，每条流水线仍只有一个在飞指令。`L1_COPY_UB`、`L1_LOOP_ENHANCE` 等操作按输入的 `pipe` 和 `cycles` 执行，不被误当作 DDR COPY。

第 2 步的最晚使用 victim 规则、第 3 步的乱序/FIFO/内存复用规则没有修改。核内顺序优化仍通过标准方案表达：`pipeline.cpp` 的候选将每个非 DDR COPY 算子作为单独子图，再给出每核子图顺序。搜索也保留较粗分块候选，只有完整评估更好的方案才会保留。目标按 `(makespan, added_copy_bytes)` 比较，优先执行时间，其次数据搬运量。

## 10 个用例在共享的 5 分钟预算内运行

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

换成实际计算图时，替换第一个路径即可。省略 `--config` 会读取计算图同目录的 `config.txt`；批量模式读取输入目录中的配置。建议明确传入已按备注更新的 `problem2/config.txt`。

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

原有六个开发入口在显式构建后位于 `build/bin/`（默认只构建 `npu_sched`）：

| 程序 | 用途 |
|---|---|
| `problem2.exe` | 问题 2 的完整搜索和提交入口 |
| `multicore_cut_evaluate_problem_1.exe` | 问题 1 / 场景 A 独立评估 |
| `multicore_cut_evaluate_problem_2.exe` | 问题 2 / 场景 B 独立评估 |
| `multicore_cut_evaluate_problem_3.exe` | 问题 3 / 带缓存的独立评估 |
| `singlecore_evaluate.exe` | 单核基线评估 |
| `stub_multicore_cut_and_schedule.exe` | 生成随机示例方案 |

`parity_runner.exe` 是测试辅助入口。不同平台或多配置构建器的可执行文件后缀、配置子目录可能不同。

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
| `problem2/official_code/multicore_cut_evaluate_problem_1` | 场景 A |
| `problem2/official_code/multicore_cut_evaluate_problem_2` | 场景 B |
| `problem2/official_code/multicore_cut_evaluate_problem_3` | 场景 B 与共享只读缓存 |
| `problem2/official_code/singlecore_evaluate` | 单核方案及评分 |
| `problem2/official_code/stub_multicore_cut_and_schedule` | 随机示例方案及方案派生信息 |

额外的 `common.hpp` 集中公共类型与五条 Pipe 的硬件表；`batch.cpp/.hpp` 管理整批时间预算；`python_order.hpp` 复现影响内存复用的 Python 整数集合顺序；`generator.hpp` 用 C++17 标准线程和条件变量保存多路线生成器的推进位置；`utf8_entry.hpp` 处理 Windows 中文参数及两系统的程序路径；`official_code/cli_main.cpp` 为各评估程序提供薄入口，算法仍在各自原名文件中。

生成器每次只唤醒一条路线，调用者等待它交回候选，其余路线休眠；不会按本机 CPU 数量启动并行搜索。取消生成器时先通知退出并等待线程结束，再释放外部数据。

建议阅读顺序：`main.cpp` → `core.hpp` → `search.cpp` → `official.cpp` → `official_code/schedule_step1/2/3.cpp`。图中的 **op** 是算子，**tensor** 是张量，**block/subgraph** 是子图块，**core** 是核心，**pipe** 是流水线。场景 B 中同核全部子图合为一个 **Task**。

## 常用参数与复评

- `-n 1..5`：**待模拟芯片的核心数**，不代表求解程序使用的电脑 CPU 核数。1 核采用整图方案并正式评分；2–5 核搜索优化。
- `--time-limit SECONDS` / `--budget SECONDS`：单例时限，范围 `(1, 1200]`。单图默认 **25 秒**；目录模式默认动态均分余时，显式设置则作为单例上限。
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

问题 1、3 使用同样的图与方案参数，换成对应程序即可；单核评估入口只需图路径。各入口支持 `--help`。

检查点会校验输入和配置的 SHA-256。C++ 评估模块及公共硬件表的源码指纹在编译时写入程序，移动程序不影响继承。也接受来源与 `original_official_hashes.json` 一致、且输入和配置哈希均一致的 Python 检查点，但必须重新评估；旧容量/等待参数的检查点不能冒充当前参数的结果。修改评估模块或硬件表后须重新编译，旧 C++ 检查点会因指纹不符而拒绝。

## 已执行的对照验证

对比完整结果结构中的数组顺序、时间线、评分、搬运量、峰值和缓存记录，不只检查最终 makespan。本次新增约束的回归测试包括：

- 20 组小型 DAG，其中 4 组包含 `PIPE_FIX`：跨核搬运、直接依赖、三种缓存容量、执行分析、三类邻域与核心启用候选一致。
- 6 组固定评估次数搜索：剔除耗时字段后，候选历史、统计项、最终评分和最优方案一致。
- 2 组更长的搜索：确认进入路线探索，并用原 Python 评估器独立复核完整评分。路线分配使用实测耗时，因此此阶段不要求跨语言的候选出现顺序一致。
- C++17 生成器的延迟启动、暂停/恢复、移动、取消清理、异常传播和截止时间传递。
- 中文路径、六个独立入口、检查点继承、2 秒限时、错误输入不覆盖旧结果。
- 独立 `PIPE_FIX` 与 M/V 重叠执行、同一 Pipe 串行、1 MiB 的 L1 张量、COPY `-1` 占位、非法负耗时和未知 Pipe 拒绝。
- 原生批量入口处理任意图名、共享时限、输出字段检查、失败不误用残留方案。
- 此前 C++20 版的真实 10 例批量输出：逐个使用补齐硬件表的 Python 场景 B 原算法重新评分，与 C++ 检查点的**完整结果结构**全部一致；C++17 版未重跑整批。

**对照范围：** 旧 Python 附件本身没有第五条 Pipe，也拒绝 COPY `-1`。测试仅在内存中给原版补充 `PIPE_FIX` 硬件列表，并为参考输入替换 COPY 占位值，调度函数保持原样；C++ 正式入口直接读取原始图。这是基于备注的本地对照，不代表已经拿到并验证过新比赛官方评估器。原 Python 文件及下发数据均未改动。

保留同级原版目录时可重跑：

```powershell
cmake -S . -B build -DBUILD_TESTING=ON -DPROBLEM2_TEST_PYTHON=python解释器完整路径
cmake --build build --config Release -j 4
ctest --test-dir build -C Release --output-on-failure
python tests/check_hackathon.py
python tests/compare_original.py
```

复核本次实际批量结果（需保留对应的工作目录）：

```powershell
python tests/verify_batch.py output/hackathon_cores_4/batch_report.json --inputs ../hackathon_data --work-dir build/hackathon_cores_4_work_v2
```

大数据中的原 Python 场景 A 评估较慢，第二条命令可能需要较长时间。`python tests/compare_original.py 1` 只跑首例；`--start 7` 从第 8 例开始。测试需要 Python 3.11+，不安装任何额外 Python 包。Windows 上固定评估次数测试只替换原版不支持的 `SIGALRM` 外壳，不修改搜索算法。

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
