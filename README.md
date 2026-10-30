# Hackathon 多核排程實驗

- **当前维护工程：[前辈代码_cpp/](前辈代码_cpp/README.md)**，在原文件内适配 `npu_sched`，保留完整算法和所有模块。
- `17/`、`17.zip` 是旧精简草稿，已停止沿用，按用户要求暂不删除；最终提交包等待用户确认后再整理。
- **新電腦／GPT 接手：先讀 [HANDOFF_zh.md](HANDOFF_zh.md)。**
- 新規範提交版：[舊算法](hackathon_v3/submission/README.md)、[模組化新算法](前辈代码_cpp/submission/README.md)，只打包各自的 `submission/`。
- 修改與重測紀錄：[SUBMISSION_REVIEW_20261030.md](SUBMISSION_REVIEW_20261030.md)。
- 舊算法研究版、自製評分器與測試：[hackathon_v3/](hackathon_v3/README_zh.md)；模組化研究版與原評分器移植：[前辈代码_cpp/](前辈代码_cpp/README.md)。
- 新測試資料：`hackathon_data/case_00.json` 至 `case_09.json`。
- 數模比賽原評分器：`前辈代码/problem2/official_code/`。
- 題目與參考材料：`材料/`。

目前配置以使用者更新為準：L1=1.5 MiB、UB=128 KiB、DDR=60 bytes/cycle、同步=1000 cycles。
新規範驗收核心數為 2、3、4、5；全部分數均需區分本地模型、原版相容性測試與正式比賽成績。
