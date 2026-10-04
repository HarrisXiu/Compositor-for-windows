# AI1 跨机器离线自测试

2026-10-04 整合后，源码启动脚本与打包工具运行九组测试，包含 C1–C5 画布测试。2026-10-03 的现有完整包保留在 `Compositor-ai1` 工作区，仍运行八组；本轮没有重新生成完整包。整合验证见 [报告](AI1_INTEGRATION_REPORT.md)。

完整包包含所有四种模型、参考数据、应用、测试工具和运行库。无需旧包或增量包，也不会弹出目录选择框。解压后打开文件夹，双击“开始测试.cmd”；结束后带回同目录中的 results-日期时间.zip。

将整个 `AI1-complete-self-test.zip` 复制到 Windows 10/11 x64 电脑，完整解压到本地磁盘。双击 `开始测试.cmd`，或在 PowerShell 中运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\self-test.ps1
```

不需要安装 Python、Qt、Visual Studio，也不需要联网或管理员权限。测试开始时校验所有文件 SHA256；不要只复制 EXE。建议接通电源、关闭其他大型计算程序，并保证数 GB 可用磁盘空间。完整模型的 CPU 检查可能需要数分钟。

当前 GPU 支持范围仅为 **Intel 核显 DirectML**。NVIDIA 和 AMD 不运行 DirectML，使用 CPU；NVIDIA CUDA 是后续工作，此包尚未实现。若同机存在可用 Intel 核显，默认优先使用它。

默认运行运行库/Schannel HTTPS 检查、基础测试（合并后新包为九组，旧包为八组）、应用启动、所有模型的 CPU 精度、Lite CPU 测速，然后逐一验证受支持的 Intel 核显。其他适配器记录为 `skipped`，注明超出当前范围。没有受支持的 Intel 核显时自动验证 CPU 回退及诊断原因，不会运行 NVIDIA/AMD 的 DirectML 失败复测。

Intel 核显使用已验证的通用 DirectML 算子路径。每个模型启动独立进程；`dml` 模式不会静默回退整个会话，Profiler 必须确认 GPU 节点执行。BiRefNet 系列每张图运行三次，每次都校验输出。Intel GPU 检查失败时才执行自动定位，并保留原始失败。

只有 CPU 或旧显卡的电脑：

```powershell
.\self-test.ps1 -CpuOnly
```

指定显卡索引或线程数：

```powershell
.\self-test.ps1 -AdapterIndex 0 -Threads 8
```

索引以测试生成的 `adapters.json` 为准，`directml_supported` 表示是否属于当前 Intel 核显范围。`directx12` 仅记录硬件能力，不等于本应用启用该适配器。选中 NVIDIA/AMD 或未发现 Intel 核显时验证 CPU 回退，GPU 项标记 `skipped`，不能算 Intel GPU 验收通过。`-CpuOnly` 主动跳过 GPU；只有硬件清单确认无 DX12 时才属于无 DX12 实机证据。`-SkipCpu` 仅用于已有 CPU 回归证据时的复测。

每次测试创建独立 `results-日期时间/` 文件夹和同名 ZIP。测试结束显示需要带回的 ZIP 路径。请带回该 **results ZIP**，其中含：

- `summary.json`：每项状态和退出码，失败不会伪装成通过。
- `hardware.json` / `adapters.json`：CPU、显卡和驱动。
- CPU/DirectML JSON：模型哈希、每项误差、蒙版 IoU/概率误差、候选蒙版身份、初始化和运行耗时。
- Profiler、对应测试日志、TLS 后端信息和模型管理对话框截图。

返回 ZIP 不包含权重或参考张量，也不会自动上传。日志含硬件/驱动信息及本地路径。测试是 ONNX 数值一致性和运行库/基础功能回归，不等于有标注分割质量、头发/肖像质量、Mac 视觉一致性或统一速度验收。此包中的上游 SAM 2 三张样本图仅用于本地验证，不能放入面向用户的模型发布资产。

源码中的 `windows/prepare_ai_delivery.py` 用标准库组装本地包；应用运行与另一台电脑上的自测试都不依赖 Python。默认包含 Lite、SAM 2、MobileSAM；生成包时的 `--include-full-birefnet` 可额外包含完整 BiRefNet。

完整包已合并 GPU 失败后的定位复测，不需要另取小型诊断包。历史 NVIDIA/AMD DirectML 失败记录仍保留，不计为通过；按最新范围，它们已退出 DirectML 验收，CUDA 留待后续。当前完整验收仍需 CPU/基础回归以及适用的 Intel 核显检查。
