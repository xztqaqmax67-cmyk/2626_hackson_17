# 17队最终交付状态

## 当前决定与用户授权
- 用户要求为17队按 `材料/提交要求文档.md` 整理自主源码、重要中文注释、可直接提交的17.zip，并逐版本上传GitHub。
- **最新明确指令：不用验证，直接按要求文档和用户要求改完压缩包。停止新增基准、CI准备和所有测试，不再补跑0.8.3或0.8.4验证。** 当前状态 **final_pending_pack_upload**。
- **最终版本0.8.4采用20秒纯搜索计时模式**：Graph解析后开始计时，包含seed构造与追加搜索，不包含写出；没有0.15秒或其他预留。20秒搜索预算不等于CLI端到端总耗时上限。
- 用户明确接受重复输出可能不一致，不再以逐字节确定性作为本轮用户验收门槛。应如实说明该例外与未验证状态，不能声称赛事方已豁免规则，也不能声称最终版重复一致或通过完整官评。
- 0.8.2/0.8.3仅为已测算法与整理基础；0.8.4曾尝试的small=2/medium=1固定轮数模式已废弃，不是最终timed模式。
- heartbeat `17` 与 `17-05` 最后已知仍 ACTIVE，最终完成后暂停；不要提前报告完成。原工作区用户改动全部保留，不整体git add/checkout/reset。

## 已发布版本与核实结果
- 仓库 `xztqaqmax67-cmyk/2626_hackson_17`。GitHub连接 `link_6ae4013b7f30819180f5ccdd07b40511`，账号 xztqaqmax67-cmyk。
- **0.8.1 已上传核实**：源码提交 `97439cbcb1e173b16699866f7793bcd66652ef00`；分支 `codex/v0.8.1` 当前头为CI工作流提交 `638a4831a83d664892b66536083c46ae83da5d3d`。冻结目录 `overnight/releases/v0.8.1/dag_chain_solver_v8_1`。
- **0.8.1 GitHub CI成功**：run `41087180788`，https://github.com/xztqaqmax67-cmyk/2626_hackson_17/actions/runs/41087180788 。已再次核实 completed/success；Ubuntu22.04 GCC11、Windows2022 MSVC构建、C++测试、CLI和图契约均通过。它不替代最终0.8.4/ZIP CI，也不代表远端两个平台跑过全部40组官评。
- **0.8.2 已上传且59/59文件核实**：分支 `codex/v0.8.2`，提交 `4a57d3934e25a6710f6e476e9168b6a1cc66a261`，源码子树 `3695fd10c4667e933097f1d28f6dd46b1debc3e6`。全部Git blob SHA与 `overnight/releases/v0.8.2/upload_source.json` 一致，核对快照 `overnight/final_ci_tools/v0.8.2_remote_tree.json`。
- **0.8.3 已发布，双平台ZIP CI成功**：分支 `codex/v0.8.3`，源码提交 `5e88f95947d085fb7a87e197e299b900923ac272`，CI提交 `6b715f6c032dd138eb0234533060e63e78d7cc61`。主线程核实 run `41140949396` 的Linux GCC11与Windows2022 MSVC两项均成功： https://github.com/xztqaqmax67-cmyk/2626_hackson_17/actions/runs/41140949396 。此记录属于0.8.3中间ZIP，不替代0.8.4验收。
- 0.8.2冻结目录 `overnight/releases/v0.8.2/dag_chain_solver_v8_2`。早期upload.json是55个项目文件加旧workflow；实际upload_source.json是59个源码/报告文件，不含workflow。不要再用旧清单误判4个新增报告。
- 旧整批create_commit曾审核超时，**不是拒绝**；上述版本后续已发布完成。旧cell57和逐文件上传状态过时，不要重复create_file。
- `overnight/repo/.git` 是未完整checkout的工作树，不可整体提交；用connector精确发布选定文件，不覆盖main或用户其他版本。

## 完整验证与选择依据
- v8起始提交 `1de70c256d14d3ab202906ac0c35711b9077cbdb`，历史均值1.933303；v6.3 `b4fc18b61555fa6f75a19229f6c01f8dd331e224`，历史均值1.989137，但墙钟截断不满足重复确定性，不能直接提交。
- **0.8.1**：`overnight/runs/v8_1/report.json`，40/40官方合法、40/40两次一致、64/64合成通过；均值 **1.975512239591×**，较v8 +2.1832558%；最大23.106秒，采样159.95MiB/5线程。
- **0.8.2**：`overnight/runs/v8_2/report.json`，40/40官方合法、40/40两次一致，共80次生成；`overnight/synthetic_v8_2/report.json` 64/64官方合法且重复一致。均值 **1.995020387979×**，较0.8.1 +0.987498%，较v8 +3.192314%；最大 **25.331秒**，采样 **159.54MiB/5线程**。
- 0.8.2相对0.8.1为12组cycles改善、4回退、24持平，总COPY增加0.613406%；相对v8为24改善、2回退、14持平。不能声称逐点全面改善或搬运也下降。
- 主线程复核 **v10全40组均值1.949478**，低于0.8.2；正式执行链含官方规则移植，故不整体吸收；0.8.4只基于原自主算法缩减固定预算。
- compact_consumer：4组0胜2平2负，子集-1.90938%，拒绝；wider_work：8/4轮数的5组均持平，无收益，不纳入；inherited_cursor：5组2胜2平1负、子集+0.5974%，未完整40组，不纳入。
- 正式算法最多4条独立工作、固定配额与归并顺序；硬件常量不变。`official_spill.*` / `spill_feedback.*` 不进正式目标或ZIP，自主refine估价保留。

## 当前0.8.4工程与验证边界
- 最终工作工程 `overnight/final_stage_v8_4/17`。模式是20秒timed纯搜索，Graph解析后启动，含seed与追加搜索，不含写出，无预留；主线程按此完成代码与压缩包。
- **用户要求跳过验证。** 不启动编译、solver、官评、契约、合成、CI或新增脚本检查。0.8.4最终timed模式没有本轮验收成绩，合法性、资源、实际耗时和重复性均不得写成已通过。
- 旧small=2/medium=1固定模式的case_05/n2约13.656秒筛查已废弃；只能作为旧实验记录，不能当成最终timed模式的成绩或20秒证明。该实验的session79464也不代表当前最终基准。
- 0.8.3矩阵停于29/40，保留 `overnight/runs/final_v8_3/report.json`：已完成29组合法且重复一致、最大25.7756175秒；11组未完成，不补跑。额外等待进程session33263已取消，未启动后续契约或64组合成测试。
- `overnight/final_acceptance_extra.json` 的旧waiting状态已失效，不表示测试仍运行或通过。根目录17/、17.zip、17.manifest.json在主线程重新打包前仍可能是0.8.3中间产物，最终身份以新包manifest为准。
- 打包工具 `overnight/package_final.py`；0.8.3整理工程 `overnight/final_stage/17` 保留，旧根ZIP `overnight/legacy_17_before_final.zip` 保留。

## 已停止的基准与CI准备工作
- 新 `overnight/benchmark_timed20.py` 已写入草稿，支持--repetitions 1或2（默认1），设计上每次单独官评、两次取均值而非最佳值；determinism与合法性/资源分开。**尚未检查或运行，用户要求跳过验证后立即停止，不将此工具草稿当验收证据。** 原benchmark.py未改。
- `overnight/final20_ci_tools/contracts.py` 与cli_regressions.py已写入timed模式调整：两次各自检查结构、保存一致性记录，失败时输出保护断言保留。rules_contracts.py无强制重复一致断言，未改。**这些最新调整未检查、未运行。**
- 0.8.4独立workflow路径 `overnight/final20-validation.yml`，远端预定 `.github/workflows/final20-validation.yml`，分支codex/v0.8.4；用户要求跳过验证后不继续准备或触发CI。旧0.8.3工具/工作流保留。
- 0.8.1及0.8.3的既有双平台CI成功属于对应旧版本，不能作为0.8.4最终ZIP的成功记录。

## 环境与交付说明
- 本机GCC `C:/Program Files (x86)/mingw-w64-gcc-13.2-stable-r40/bin/g++.exe`；Python `C:/Users/max67/miniconda3/python.exe`；工作区CMake/Ninja在overnight/tooling和overnight/final_tools。旧WinGet GCC16/MSVC缓存路径在本机不存在，勿引用。
- 官方源码/数据 `前辈代码_cpp/official_evaluator`，以SOURCE_MANIFEST.json核对，绝不修改，始终在ZIP外。
- 根 `17队_答辩说明.md`、`17队_版本对比与验收报告.md` 原先基于0.8.2/0.8.3撰写；主线程需统一改为最终实际选定的0.8.4版本、固定预算、SHA、CI和成绩。
- 最终ZIP唯一根17/，仅CMakeLists.txt、README.md、自主src；README明确无第三方库。禁止官方代码/数据、答案、构建产物、测试脚本、Git/IDE目录，大小≤20MiB。
- 用户目标是20秒纯搜索预算；CLI端到端耗时、官方硬超时与工具原有超时设置另算。公开测试与资源采样不能证明任意隐藏图或机器都满足20秒或90秒；最终如实说明覆盖，不虚构全局最优或隐藏用例保证。
- 按用户要求完成打包、上传和文档，统一反馈17.zip、答辩说明、版本报告及GitHub版本链接，并明确最终timed版跳过验证；随后停止两项heartbeat。


## 最终交付 0.8.4（本节覆盖前述待办）
- 用户最终选择：20 秒纯搜索预算，接受重复不一致；随后明确要求不再验证，直接按文档和其要求修改并打包。不得自动续跑矩阵、合成图或 CI。
- 已生成根目录 17/ 与 17.zip，22 文件、69,036 字节；SHA256 d8ffe7336cc8319fd3a499fd9455731d266b27997f7ad8a919d320f77b05642f。
- 正式入口 solve_for_budget；Graph 解析后启动计时，包含基础种子与追加搜索，输出预留 0 秒；读图、写出与打包另算。
- 协作式20秒预算，基础种子不可中断；不是硬实时20.000秒保证。可能不满足v3.0逐字节确定性，用户已接受此项偏离，不代表组委会豁免。
- 本版未编译、未跑测试、没有最终官评分数；旧0.8.2/0.8.3成绩和CI不得冒充0.8.4验证。
- GitHub 分支 codex/v0.8.4；源码与ZIP提交 fa9e20d4f2ed3eb3423190d070d73bd4315cf2e2；最终说明文档另追加同版本提交。
- 根目录两份中文文档已改为0.8.4：17队_答辩说明.md、17队_版本对比与验收报告.md。用户原工作区改动未整体提交或覆盖。
- 原0.8.3包保存在 overnight/archived_v0.8.3_submission；最新20秒包应以根17.zip为准。
