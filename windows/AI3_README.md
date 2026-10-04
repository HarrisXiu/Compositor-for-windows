# AI3：背景移除与精细蒙版

## 当前结果

`codex/ai3` 基于 AI2 提交 `0fdb5de`，完成 BiRefNet Lite 背景移除和引导滤波精细模式。版本仍为 0.4.0，`.comp` 格式仍为 11；只写入原有图层蒙版字段，不新增文件格式。原始图层像素、文字/形状元数据和变换保持不变。

模型、运行库和许可沿用 AI1/AI2：FP32 BiRefNet Lite、固定大小/SHA256、完整许可校验、后台执行与取消。应用运行不需要 Python。GPU 仍只支持受支持的 Intel 核显 DirectML，其他显卡使用 CPU；NVIDIA CUDA 留待后续。

## 使用

1. 在 **帮助 > AI 模型…** 导入已验证的 `birefnet-lite.onnx`。正式下载地址尚未发布，继续使用本地导入。
2. 选择需要处理的像素图层，打开 **滤镜 > 移除背景…**。推理输入是当前图层自己的原始像素，与 AI2 对可见合成图的主体选择不同；文件夹和无像素调整图层不能执行该命令。
3. **基础**：sigmoid 概率按像素中心双线性缩放为 8 位软蒙版，保留透明过渡，不使用 AI2 的二值阈值。
4. **精细**：细化 0–40 图层像素，对比度 0–100%，边缘偏移 −10 至 +10 图层像素；默认 12 / 25 / 0。细化按图层图像的亮度边缘调整蒙版；负偏移收缩，正偏移扩展。可勾选「显示蒙版」查看灰度结果。
5. 点击 **确定** 后后台计算完整分辨率蒙版，成为一次可撤销的图层编辑。**取消**、关闭窗口或 `Esc` 不修改文档，也不等待推理工作线程退出。

已有选区时只修改选中区域；羽化覆盖参与连续混合，未选中区域保留原有蒙版，没有原蒙版则保留为白色。已有蒙版与新主体覆盖相乘，原先隐藏的区域不会被恢复。任意旧蒙版放置先映射到当前图层像素网格，结果移除独立放置矩阵，启用并与图层链接；旧蒙版放置和链接状态可随撤销完整恢复。这一步会把原先不链接的蒙版转换成跟随图层的蒙版，以保持当前图像的覆盖位置。

确认后切换到蒙版编辑目标，可继续手工修改蒙版或关闭/移除蒙版来恢复背景。预览在独立对话框显示棋盘格透明背景，不写入文档、撤销栈或保存文件。

## 计算与响应

- 一个对话框只推理一次原始软蒙版；切换基础/精细或调整参数复用它。
- 参数变化合并请求，只显示最新结果；细化计算可取消，关闭对话框使任务失效。来源图像发生外部变化时拒绝旧任务提交。
- 预览最长边不超过 1400 像素，细化和偏移半径按比例调整；确定时重新执行完整分辨率后处理，因此预览和最终细节可以略有不同。
- 引导滤波沿用原版 He/Sun/Tang 的局部线性模型，epsilon 为 1e−4，亮度为 0.2126R + 0.7152G + 0.0722B。每个 512 像素块携带 2r 邻域，保持整图计算的边界和接缝；临时浮点平面按块分配，而不是按整幅大图分配。
- 顺序为引导滤波、边缘偏移、对比度。偏移用 sigma = abs(shift)/2 的夹边高斯和 0.75 / 0.25 阈值带；对比度斜率为 1 / max(0.02, 1 − 0.98 × strength)。

## 验证

| 项目 | 结果 |
| --- | --- |
| x64 Release 构建 | Visual Studio 2026 / Qt 6.10.2 成功 |
| 联合回归 | AI2 错误回滚用例改为等待已完成的错误状态，避免 Windows 固定延时抖动；十一组 CTest 全部通过，原 AI2、AI1、S1/S3 和编辑功能回归保留 |
| AI3 默认用例 | 19 项通过，3 个真实模型用例默认跳过；已另外显式执行实图验收 |
| 独立算法参考 | 朴素全邻域实现验证边界、单像素和分块接缝；最大误差 ≤ 1 灰度级 |
| CPU 实图 | 三张图，基础最大误差 1 / 255；精细最大误差 2 / 255 |
| Intel 核显实图 | 三张图，基础最大误差 1 / 255；精细最大误差 2 / 255；强制 DirectML 策略 |
| 真实编辑器事务 | CPU / Intel 各三条菜单→预览→完整确认→撤销→重做流程通过，原始像素保留 |
| 保存与交互 | 已有蒙版、放置、链接后旋转/缩放/移动、变换选区、羽化、取消/失败/过期结果、模型缓存、保存重开通过 |
| 界面与启动 | Windows 平台中英日布局和实际应用启动检查通过 |
| 交付工具 | 新增 AI3 可执行程序与自测试项，PowerShell / Python 语法检查通过；未重新制作 ZIP |

实图为 AI1 保存的 truck / cars / groceries，参考来自原有 Python ONNX logits 和独立 NumPy 后处理；参考工具检查压缩张量大小与 SHA256。基础和精细均比较完整 8 位蒙版，而不是只比较二值区域。精细实图验收参数为 12 / 25 / 0；其他参数的算法性质由合成测试覆盖。

这些检查验证实现对数值参考的一致性，不是人工真值、发丝抠图质量或 Mac 视觉对照验收。与原版采用不同模型和图像库；引导滤波不能补回模型完全遗漏的对象。ONNX Runtime 允许部分算子在 CPU 上执行，后端可能记为 `DirectML+CPU`；不代表 NVIDIA / AMD DirectML 支持。

## 复查与记录

源码：`ai_background.*`、`ai_background_dialog.cpp` 和 `ai_selection.*` 的软蒙版路径；回归：`windows/tests/ai_background_tests.cpp`。工作树 `artifacts/` 保存构建/CTest 日志、CPU/Intel QtTest 日志、六份 JSON、基础/精细蒙版、透明预览和中英日界面截图。模型与缓存继续留在原位置，不进入 Git。

```powershell
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
# 下面需要既有模型、AI1 references.json/张量和固定依赖的开发 Python 环境。
python -B windows/tests/background_reference.py --references '<AI1参考目录>' --output artifacts/ai3-python-reference
$env:QT_QPA_PLATFORM = 'offscreen'
$env:COMPOSITOR_AI_SELECTION_MODEL_ROOT = '<包含 birefnet-lite 子目录和许可的模型缓存根目录>'
$env:COMPOSITOR_AI_SELECTION_REFERENCES = '<AI1参考图片目录>'
$env:COMPOSITOR_AI_BACKGROUND_REFERENCES = '<独立参考输出目录>'
$env:COMPOSITOR_AI_SELECTION_PROVIDER = 'cpu' # 或 dml，强制受支持的 Intel 核显
.\build\Release\compositor_ai_background_tests.exe realBackground -o artifacts/ai3-real-tests.txt,txt
```

后续仍包括人工抠图质量/Mac 对照、更多 Intel 设备、无 DX12 GPU 实机回退、NVIDIA CUDA、模型正式下载和便携包发布验收。本轮不推送、不合入主分支、不创建 Release 或上传资产。
