# 最终 ZIP 的独立 CI 工具

这些工具保存在提交 ZIP 外。CI 只检出 `17.zip`、`17.manifest.json` 和本工具目录，然后校验 ZIP 身份、白名单和每个文件 SHA-256，解压到此前不存在的目录。两个平台都仅配置、编译解压得到的 `17/`，不使用仓库中的其他算法源码。

- `contracts.py`：CLI 选项、退出码、Unicode/空格路径、输出确定性与输入输出保护；版本期待为 `0.8.3`。
- `cli_regressions.py`：临时路径冲突、原子替换及错误路径回归；版本期待为 `0.8.3`。
- `rules_contracts.py`：26 项合成图输入契约检查。
- `inspect_zip.py`：先验 ZIP/manifest，再干净解压；`--check-only` 可只读检查，不写文件。

三个契约脚本复制自已测 0.8.2 的对应测试，仅更新版本期待及临时目录前缀。它们使用自身构造的小图，不导入官方评估器，也不依赖公开测试数据。CI 的构建/契约成功不等于已经在两个远端平台重跑公开 40 组官方评估。

发布时将 `overnight/final-validation.yml` 写入同一提交的 `.github/workflows/final-validation.yml`，并上传本目录、最终 `17.zip` 和它对应的 `17.manifest.json`。源包内容发生变化后必须重新生成 manifest，再重新触发 CI；不能复用旧 ZIP 的成功记录。

`v0.8.2_remote_tree.json` 仅是此前源码发布的只读核对记录，不参与构建、测试或压缩包内容。
