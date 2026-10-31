# 17 队多核调度方案生成程序 0.8.3

本程序面向场景 B（问题 2）的多核计算图调度。输入计算图和核心数，输出操作到子图的映射及各核心的执行顺序，以降低 Makespan 和额外数据搬运量。算法与 JSON 解析均使用 C++17 和标准库实现。

## 构建和运行

Linux：GCC 9.4 及以上，CMake 3.16 及以上。在本目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/npu_sched "/path with spaces/graph.json" -n 4 -o "/path with spaces/plan.json"
```

Windows：Visual Studio 2019 16.8 / Visual Studio 2022，CMake 3.20 及以上。在本目录执行：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release -j
.\build\bin\npu_sched.exe "C:\input with spaces\graph.json" -n 4 -o "C:\output with spaces\plan.json"
```

可执行文件位于 `build/bin/`。输入、输出路径支持相对路径、绝对路径、空格和中文；相对路径以程序启动目录为基准。

## 程序接口

```text
npu_sched <graph.json> [-n N] [-o plan.json]
npu_sched --version
```

`-n` / `--cores` 的合法值为 2、3、4、5，默认 4；`-o` / `--output` 指定方案文件。支持 `--option value` 与 `--option=value`。省略输出时，写到输入目录的 `<case>_multicore_res.json`，输出目录不存在时自动创建。`--version` 输出 `dag-chain-solver 0.8.3`。

退出码为：0 成功；1 内部错误；2 命令行错误；3 输入错误；4 输出失败；5 无法获得合法方案。错误信息写入 stderr，不读取 stdin，不等待交互。

硬件常量固定为 L1 **1572864 bytes**、UB **131072 bytes**、DDR 总带宽 **60 bytes/cycle**、跨核等待 **1000 cycles**。这些值不接受命令行、环境变量或文件覆盖。

输出 JSON 顶层包含 `node_to_subgraph` 和 `core_schedules`。每个非 `COPY_IN` / `COPY_OUT` 操作对应一个子图；全部子图恰好分配一次，外层核心队列长度等于 N，允许空核心。同核子图合并为一个 Task 复用核内数据，跨核依赖通过数据搬运与同步衔接。

## 算法思路与过程

1. **计算图建模。** 解析 Tensor 和 Op 的生产、消费关系，收缩边界 COPY，建立计算操作 DAG。计算关键路径和拓扑顺序；共享 Tensor 的多个写入者保持同核，避免将写入关系拆开。
2. **链分解与放置。** 选取关键前驱形成不分叉的计算链，再按负载与通信权重切成链段。结合核心管道负载和跨核通信代价进行放置，兼顾并行度和数据复用。
3. **多初解。** 对不同核心预算生成候选，结合区域划分、关键路径、依赖等待空隙及内存压力策略，避免搜索只围绕单一起点。所有决策由输入图和 N 驱动。
4. **分层评估。** 先用快速模型估计管道负载、跨核通信和内存压力，筛选候选；再对少量候选进行共享 DDR、多管道和依赖等待的事件估价，以此比较方案的执行效率与搬运开销。
5. **固定工作量改进。** 从多个已完成候选继续尝试链段迁移、消费者组迁移、合法拓扑重排及内存窗口策略。每条路线使用固定的搜索额度，较大图缩减额度，始终保留完整可用候选。
6. **归并与写出。** 独立工作完成后按固定顺序和方案比较键合并结果，线程完成先后不参与选择。验证操作覆盖和依赖顺序后，完整写出临时文件，再替换目标文件。

### 搜索额度与并行执行

搜索工作量由输入图规模和核心数确定。规模量为计算操作数、张量数和边数之和：

| 图规模量 | 每条路线的追加搜索轮数 |
|---|---:|
| 不超过 80,000 | 4 |
| 超过 80,000 且不超过 200,000 | 2 |
| 超过 200,000 | 0，使用多初解阶段的最优候选 |

追加搜索采用最多四条独立路线，每条路线每轮最多粗估 12 个候选、细化 3 个候选，搜索空间耗尽时结束。候选以统一比较键排序，分数相同时继续比较核心分配和操作顺序。

每个并行阶段最多创建三个额外的 `std::thread`，调用线程承担一条路线；各阶段顺序执行。全部路线完成后按固定下标归并结果，使候选选择与线程执行快慢无关。

## 源码结构

| 文件 | 作用 |
|---|---|
| `src/main.cpp` | 命令行、输入输出、退出码和文件完整写出 |
| `src/model.hpp` | JSON 解析、计算图、硬件常量、拓扑与快速估价 |
| `src/chain.cpp` | 链分解、初始方案与各核心预算搜索 |
| `src/regions.cpp` | 区域划分与核心配额候选 |
| `src/slack.cpp` | 利用依赖等待空隙生成候选 |
| `src/spill.cpp` | 内存压力导向的自主重排策略 |
| `src/anytime.cpp` | 多起点候选搜索与确定性归并 |
| `src/tensor_neighborhood.cpp` | 消费者组迁移和写者闭包 |
| `src/refine.cpp` | 共享 DDR 与多管道的内部事件估价 |
| `src/search_limits.hpp` | 固定搜索次数与图规模配额 |
| `src/parallel_jobs.hpp` | 独立并行工作与固定顺序收集 |
| `src/memory_grid.hpp` | 内存排序参数的固定交错枚举 |
| `src/solution_key.hpp` | 候选比较与分数相同时的固定排序规则 |

## 第三方库清单

**无第三方库。** JSON 解析与调度算法由工程源码实现，依赖 C++17 标准库。Windows 下的字符转换和文件替换使用系统接口，通过平台宏适配。
