# 17 队最终提交版：0.8.3

最终提交包为 [17.zip](17.zip)，对应源码在 [17/](17/README.md)。下载单独的 `17.zip` 用于提交；仓库中的研究目录、测试与报告不属于提交包。

- [构建、调用接口与算法说明](17/README.md)
- [逐项要求核对及已知限制](reports/v0.8.3-final/COMPLIANCE.md)
- [40 组公开数据复测记录](reports/v0.8.3-final/public_matrix.json)
- [附加 12 项要求检查](reports/v0.8.3-final/extra_checks.json)
- [提交要求 v3.0 原文](reports/v0.8.3-final/REQUIREMENTS.md)
- [压缩包与源码 SHA-256 清单](17.manifest.json)
- [GCC 11 / MSVC 干净 ZIP 构建与契约检查](https://github.com/xztqaqmax67-cmyk/2626_hackson_17/actions/workflows/final-v083-validation.yml)

公开数据 10 例 × 2/3/4/5 核全部通过；同命令共执行 80 次，40 组输出逐字节一致。本地平均加速比 **1.995020388×**，最长生成 **30.690 秒**，采样峰值内存约 **185 MiB**、线程数 **5**。单核参考沿用已有记录；生成耗时受到同机其他任务影响，以上不是比赛服务器成绩。

0.8.3 使用固定工作量搜索以保持重复一致性，**没有主动的 90 秒截止机制**。公开数据全部满足时限，但不能据此保证任意更大隐藏图或更慢机器均在 90 秒内完成。源码同时接受 `PIPE_FIX`、COPY 的 `cycles=-1` 和直接 Op→Op 边，这是输入兼容扩展，不改变要求内的固定硬件参数及输出格式。

固定参数：L1=1572864 bytes，UB=131072 bytes，DDR=60 bytes/cycle，跨核等待=1000 cycles。正式入口 `npu_sched graph.json -n 4 -o plan.json`，仅接受 2～5 核。

ZIP SHA-256：`de3d43cc152984d9b0d5fd40634c1d575fed6599bd84749f3cabff7106ec7dfa`。
