# AI2：主体与对象选择

## 当前结果

AI2 已合入 `the-one-for-windows`，与今天其他功能分支的统一验收见 [2026-10-04 报告](ACCEPTANCE_REPORT_2026-10-04.md)。以下分支基线与测试数记录独立开发阶段。

`codex/ai2` 基于 S1/S3 提交 `6b18f74`（含 AI1 整合基线 `edbf0c5`），完成开发计划中的 AI2：BiRefNet 主体选择，以及 SAM 2 / MobileSAM 点击提示对象选择。应用版本保持 0.4.0，`.comp` 格式保持 11。选区、提示点和预览都是会话状态，不写入项目；像素内容保持不变。

主体使用已验证、MIT 许可的 BiRefNet Lite FP32。对象使用 SAM 2 Hiera Tiny 或 MobileSAM FP32，均沿用 AI1 的固定模型哈希与许可证。应用不需要 Python。

## 使用

1. 在 **帮助 > AI 模型…** 导入匹配的本地模型。主体需要 `birefnet-lite.onnx`；对象需要所选模型的 encoder / decoder 两个文件。管理器会检查大小、SHA256 和许可，安装后可离线使用。也可以直接点击「下载」，从 ai-models-v1 发布页获取。
2. 使用 **选择 > 选择主体**。应用对可见图像合成结果后台推理，计算过程中可以取消。成功后成为一次可撤销的选区变化，不修改图层像素，也不会让已保存的图像内容变脏。
3. 使用 **选择 > 选择对象…**，选 SAM 2 或 MobileSAM。直接在主画布点击前景对象，继续添加提示点改善结果；按住 `Alt` 点击排除背景，也可在对话框选择前景 / 背景提示。绿色「+」表示前景，红色「−」表示背景。
4. 在对象对话框选择替换 / 添加 / 减去 / 相交。预览完成后按 **确定** 或画布上的 `Enter` 确认；**取消**、关闭窗口或 `Esc` 恢复原选区。`Backspace` /「删除上一个提示点」回退提示；「清除提示点」恢复原选区。滚轮缩放，按住空格拖动画布。最多 1024 个提示点。
5. 主体命令沿用当前项目的选区组合模式。已有羽化、扩展 / 收缩和「用选区建立蒙版」可以继续处理 AI 选区。

对象模式在同一幅图像上复用 SAM 编码。快速点击时合并待处理请求，仅最新结果可以成为预览；不会为每次预览生成撤销记录。切换模型、项目或工具、编辑图像、修改选区和关闭项目都会使旧任务失效；图层面板编辑在捕获撤销快照前恢复原选区。取消不会等待工作线程退出，工作线程只持有数据副本和推理服务。

## 输出约定

- 输入是画布大小的可见图像合成结果，不包含标尺、参考线或交互叠加。
- BiRefNet：sigmoid 转概率，再按像素中心双线性缩放到画布大小，以 0.5 为前景阈值。
- SAM：按预测 IoU 选择候选；SAM 2 比较三个候选，MobileSAM 比较索引 1–3，排除独立的单蒙版 token。输出 logits > 0 为前景。
- 完全透明的源像素不进入选区。输出是画布大小的 Grayscale8 二值蒙版；需要柔边可使用羽化。
- 每次首次打开模型验证固定大小、SHA256 和完整许可内容。许可文本允许 LF / CRLF 等价换行；改字、缺失许可或模型损坏仍拒绝使用。该兼容修复没有改变模型哈希检查。

## 验证

| 项目 | 结果 |
| --- | --- |
| x64 Release 构建 | Visual Studio 2026 / Qt 6.10.2 成功 |
| 交付工具 | PowerShell / Python 脚本语法检查通过，固定依赖 Python 环境的十项模型导出测试通过 |
| 联合回归 | 十组 CTest 全部通过，包含原有编辑、S1/S3、AI1、模型管理和新增 AI2 |
| CPU 实图选区 | 三模型 × 三张图，共 21 项；全部通过，最小 IoU 1.0 |
| Intel 核显 DirectML 实图选区 | 同样 21 项；全部通过，最小 IoU 0.999969，验收线 0.995；强制 DirectML 策略，不允许整次推理静默回退 CPU |
| 同图编码复用 | SAM 各实图的正点、正负点、三点检查均只有一次编码 |
| 真实编辑器流程 | CPU / Intel 各三种模型，共六条主体菜单 / 画布对象 / 确认 / 撤销 / 重做流程；IoU 1.0，图像内容不变脏 |
| 界面 | 中英日文本、Windows 字体与布局检查通过；实际应用启动检查通过 |

模型数值参考沿用 AI1 的 `truck.png`、`cars.png`、`groceries.png` 和 Python ONNX 输出。上述 IoU 衡量实现对参考结果的一致性，不是与人工标注真值比较，也不代表发丝 / 肖像质量、Mac 视觉一致性或所有硬件验收。SAM 部分算子仍可由 ONNX Runtime 分配给 CPU，后端记为 `DirectML+CPU`。

默认 CTest 不要求下载模型；真实模型用例默认跳过，以上实图及编辑器流程已通过显式运行完成。新增事务测试覆盖确认、取消、提示合并、清空、失败回滚、面板编辑、直接选区变化、切换标签、画布取点、边界和撤销。许可回归覆盖 LF / CRLF 等价、篡改文本拒绝及损坏模型拒绝。

工作树的 `artifacts/` 保留构建日志、`ai2-ctest-final.log`、CPU / Intel QtTest 日志、18 份实图 JSON、六份编辑器 JSON，以及中英日截图。模型仍使用原保存位置，没有复制进 Git。

复查（已有模型与 Python 参考文件）：

```powershell
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
$env:QT_QPA_PLATFORM = 'offscreen'
$env:COMPOSITOR_AI_SELECTION_MODEL_ROOT = '<包含 birefnet-lite / sam2 / mobilesam 子目录的缓存根目录>'
$env:COMPOSITOR_AI_SELECTION_REFERENCES = '<references.json 和参考数据所在目录>'
$env:COMPOSITOR_AI_SELECTION_PROVIDER = 'cpu' # 或 dml，强制受支持的 Intel 核显
.\build\Release\compositor_ai_selection_tests.exe realModelSelection realEditorSelection -o artifacts/ai2-real-tests.txt,txt
```

运行用例前创建 `artifacts/`。模型缓存目录应包含 AI1 管理器安装的模型与许可；不支持任意 ONNX 替换。代码位于 `ai_selection.*`、`ai_selection_dialog.cpp` 和画布 / 编辑器接入；测试位于 `windows/tests/ai_selection_tests.cpp`。未来完整自测试交付工具已加入 AI2 测试程序和执行项，本轮没有重新制作便携 ZIP。

## 后续范围

AI3 背景移除与引导滤波细化已完成并合入主分支，见 [AI3 说明](AI3_README.md)。GPU 仍只支持受支持的 Intel 核显 DirectML，其他显卡使用 CPU；NVIDIA CUDA、更多硬件与人工质量验收留待后续。独立 AI2 开发阶段没有推送、合入或上传模型资产；当前合入状态以统一报告为准。
