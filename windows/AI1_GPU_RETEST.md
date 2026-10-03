# AI1 GPU 定位复测

这是用于已有 AI1-self-test 的小型诊断包，不含模型，不修改原包或之前的结果。

1. 把 AI1-gpu-retest.zip 带到原测试电脑，解压到原 AI1-self-test 文件夹内。
2. 确认目录是 AI1-self-test/AI1-gpu-retest/run-gpu-retest.cmd，旁边仍有原来的 bin、models、references 和 SHA256SUMS.json。
3. 接通电源，关闭游戏、模拟器和其他 GPU 计算程序。双击 run-gpu-retest.cmd。
4. 带回新生成的 gpu-retest-日期时间.zip。

无需联网、安装软件或管理员权限。首次运行会把原包中已校验的 DLL 复制到探针旁边，避免 Windows 系统中的旧版 ONNX Runtime 抢先加载。默认测所有支持 DX12 的硬件适配器，包括虚拟显示驱动暴露的重复索引；约需数分钟，失败后的定位检查可能延长时间。

原 results-20261003-180958：基础测试和 CPU 132/132 通过；Ryzen 7800X3D 的 Lite 九次平均约 3.9 秒/图。RTX 4070 SUPER 的 SAM 2/MobileSAM 通过；BiRefNet/Lite 有严重误差。AMD 核显完整 BiRefNet 出现 0x887A0006，后续模型当时未执行。通用算子诊断也失败。根因尚未确认。

新版逐模型启动独立进程，先测 Lite/SAM，最后测完整 BiRefNet；错误不会阻止其他模型运行。BiRefNet 每图运行三次，每次都检查输出。失败时补测通用算子、倒序图片、单图独立会话和自动 CPU 回退；原始失败仍计入最终状态。探针在推理前后检查 DirectML/D3D12 设备状态，设备失败时明确报错，prefer 模式尝试 CPU 回退。该保护不能保证检出所有数值错误，精度问题需以新报告为准。

仅测某个适配器：
~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-retest.ps1 -AdapterIndex 1
~~~

也可以在 PowerShell 中指定模型：
~~~powershell
.\gpu-retest.ps1 -AdapterIndex 1 -Models birefnet-lite,sam2,mobilesam
~~~

索引以新生成的 adapters.json 为准。报告包含硬件/驱动、模型哈希、每次误差、GPU Profiler、耗时和本轮系统 GPU 恢复事件；读取不到事件时记录原因。报告 ZIP 不含模型/参考张量，也不会上传。

测试维持原来的误差、IoU 和概率阈值；通过诊断路径不能覆盖默认失败。本轮是问题定位，并非完整 AI1 验收。
