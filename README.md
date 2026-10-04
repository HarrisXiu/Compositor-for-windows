# Compositor for Windows

**Windows 移植版尚未完全完成，后续将持续更新。** 当前版本为 **0.4.0 预览版**，目标平台为 Windows 10（1903 或更新版本）/11 x64。

本项目基于 [robbietilton/Compositor](https://github.com/robbietilton/Compositor)，将原有 macOS 图像合成与照片编辑流程移植到 Windows。Windows 应用采用 **C11、C++20 和 Qt 6 Widgets**，复用原项目的 C 像素算法。原 macOS 源码仍保留在仓库中。

项目继续采用 **MIT 协议**，第三方库与模型遵循各自许可。按移植范围约定，不提供自动更新功能。

## 当前能力

- 多项目标签、`.comp` 项目读写、后台保存，以及常用图片导入和 PNG/JPEG 导出。
- 图层与文件夹、24 种混合模式、蒙版与剪贴蒙版、合并、分组和跨项目复制。
- 绘画与修复、多边形套索、色彩范围、选区扩展/收缩和选区蒙版、裁剪和图像尺寸调整；调整图层、图层效果、Dither 与 Camera Raw 滤镜。
- PSD/PSB 导入与相机 RAW 开发，支持范围和转换限制见 [Windows 使用说明](windows/README.md)。
- 离线 AI 主体选择与 SAM 2 / MobileSAM 画布点击对象选择，以及软蒙版背景移除和引导滤波精细模式，支持预览、取消和撤销。
- 分块画布渲染、标尺/参考线/网格、吸附、工具选项和自定义快捷键。
- 变换：图层与蒙版的缩放/旋转手柄，Ctrl 拖动手柄自由扭曲；多个图层或文件夹整组缩放、旋转和扭曲，移动工具拖动所有选中图层，Alt 拖动复制选中内容。
- 画布内文字输入、可拖拽段落框、文字工具栏和字体映射；基础形状按矢量源缩放。
- 简体中文、English、日本語即时切换，并记住语言选择。

## 移植计划与当前进度

| 阶段 | 当前进度 |
| --- | --- |
| 基础框架、项目与核心编辑能力 | 已建立，持续完善兼容性 |
| 图层与裁剪尺寸操作（L1–L4） | 当前范围已完成 |
| 渲染与大画布优化（R1–R4） | 已整合；目标硬件性能验收待完成 |
| 画布交互、工具选项与快捷键（C1–C5） | 当前 Windows 工具范围已完成 |
| 变换（T1、T2） | 自由扭曲、整组变换、移动多个图层与 Alt 拖动复制已完成，见 [交付说明](windows/T1-T2_REPORT.md)；扭曲松手即生效，不能停在待应用状态 |
| 绘画、越界扩展、仿制与渐变（P1–P3） | 已完成并通过回归测试，见 [交付记录](windows/P1-P3_REPORT.md)；大文档与 Mac 对照验收待完成 |
| 文字与形状（X1、X2） | 画布编辑、段落框、工具栏、字体映射与矢量缩放已完成；输入法候选窗和 Mac 字体对照待人工验收 |
| 离线 AI 基础设施（AI1） | C++ 推理基础层与模型管理已合入；支持本地导入、离线缓存和 CPU 回退 |
| 选区基础完善与色彩范围（S1/S3） | 当前范围已完成，使用和验证见 [交付说明](windows/S1-S3_REPORT.md) |
| AI 主体与对象选择（AI2） | 当前范围已完成，使用与实图验收见 [AI2 说明](windows/AI2_README.md) |
| 背景移除与精细蒙版（AI3） | 当前范围已完成，使用与实图验收见 [AI3 说明](windows/AI3_README.md) |
| 浮动选区与选区变换（S2） | 待开发 |
| 高级工具、格式兼容与正式发布 | 继续开发和验收 |

合并 T1/T2、S1/S3、P1–P3、X1/X2 和 AI1–AI3 并修复 CI Unicode 路径问题后，统一 13 组测试全部通过（386 项通过、0 失败；另有 20 项需要模型、硬件、网络或基准数据的用例默认跳过），覆盖原有编辑功能、画布交互、变换、选区、绘画、文字与形状、AI 推理与模型管理；变换见 [T1/T2 交付说明](windows/T1-T2_REPORT.md)，文字与形状见 [X1/X2 交付记录](windows/X1-X2_REPORT.md)，绘画见 [P1–P3 交付记录](windows/P1-P3_REPORT.md)，AI 基础层见 [AI1 整合报告](windows/AI1_INTEGRATION_REPORT.md)。完整 Mac 功能与视觉一致性仍需验收。

2026-10-04 已核对当天全部功能分支的合入状态，并重新执行统一回归及可选实图检查；结果与验收边界见 [统一验收报告](windows/ACCEPTANCE_REPORT_2026-10-04.md)。历史分支报告中的“未合入”描述保留当时状态，以统一报告为准。

AI 当前 GPU 范围为**受支持的 Intel 核显 DirectML**，其他显卡使用 CPU；NVIDIA CUDA 留待后续。AI2 主体/对象选择和 AI3 背景移除/精细蒙版已接入，验证方法见 [AI2 说明](windows/AI2_README.md) 与 [AI3 说明](windows/AI3_README.md)。通过 **帮助 > AI 模型…** 导入匹配的本地模型；正式模型地址尚未发布，下载入口暂不启用。使用与支持范围见 [AI1 说明](windows/AI1_README.md)。

后续重点是浮动选区与选区变换（S2）、网格扭曲等剩余变换，扩大真实文件与硬件测试，再完成跨平台对照和发布验收。详细任务见 [移植进度](windows/PORTING_STATUS.md)。

## 构建与使用

开发环境需要 Visual Studio C++ 工具链、Windows SDK、CMake 和 Qt 6 的 MSVC x64 开发包。本地已验证 Qt 6.10.2。仓库根目录运行：

```powershell
.\windows\build.ps1 -QtRoot C:\Qt\6.10.2\msvc2022_64
```

该命令构建并运行测试；依赖准备、可选打包和运行说明见 [Windows README](windows/README.md)。便携包需完整解压，保留程序旁的 DLL 与插件目录，再运行 `Compositor.exe`。目前仍为开发预览，不代表正式发布已完成。

## 文档

- [统一验收：2026-10-04 全部分支](windows/ACCEPTANCE_REPORT_2026-10-04.md)
- [开发日志：2026-10-03 至 2026-10-04](windows/DEVELOPMENT_LOG.md)
- [AI1 整合报告](windows/AI1_INTEGRATION_REPORT.md) · [AI1 使用说明](windows/AI1_README.md)
- [版本变更记录](windows/CHANGELOG.md)
- [移植进度与待办](windows/PORTING_STATUS.md)
- [L1–L4](windows/L1-L4_REPORT.md) · [C1–C5](windows/C1-C5_REPORT.md) · [T1/T2](windows/T1-T2_REPORT.md) · [S1/S3](windows/S1-S3_REPORT.md) · [P1–P3](windows/P1-P3_REPORT.md) · [X1/X2](windows/X1-X2_REPORT.md) · [AI2](windows/AI2_README.md) · [AI3](windows/AI3_README.md) 交付记录
- [Windows 架构](windows/ARCHITECTURE.md) · [项目格式](docs/project-format.md)
- [第三方许可说明](windows/THIRD_PARTY_NOTICES.md) · [MIT 许可证](LICENSE)
