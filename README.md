# Compositor for Windows

**Windows 移植版尚未完全完成，后续将持续更新。** 当前版本为 **0.4.0 预览版**，目标平台为 Windows 10/11 x64。

本项目基于 [robbietilton/Compositor](https://github.com/robbietilton/Compositor)，将原有 macOS 图像合成与照片编辑流程移植到 Windows。Windows 应用采用 **C11、C++20 和 Qt 6 Widgets**，复用原项目的 C 像素算法。原 macOS 源码仍保留在仓库中。

项目继续采用 **MIT 协议**，第三方库与模型遵循各自许可。按移植范围约定。

## 当前能力

- 多项目标签、`.comp` 项目读写、后台保存，以及常用图片导入和 PNG/JPEG 导出。
- 图层与文件夹、24 种混合模式、蒙版与剪贴蒙版、合并、分组和跨项目复制。
- 绘画与修复、基础选区、裁剪和图像尺寸调整；调整图层、图层效果、Dither 与 Camera Raw 滤镜。
- PSD/PSB 导入与相机 RAW 开发，支持范围和转换限制见 [Windows 使用说明](windows/README.md)。
- 分块画布渲染、标尺/参考线/网格、吸附、工具选项和自定义快捷键。
- 简体中文、English、日本語即时切换，并记住语言选择。

## 移植计划与当前进度

| 阶段 | 当前进度 |
| --- | --- |
| 基础框架、项目与核心编辑能力 | 已建立，持续完善兼容性 |
| 图层与裁剪尺寸操作（L1–L4） | 当前范围已完成 |
| 渲染与大画布优化（R1–R4） | 已整合；目标硬件性能验收待完成 |
| 画布交互、工具选项与快捷键（C1–C5） | 当前 Windows 工具范围已完成 |
| 离线 AI 基础设施（AI1） | 主分支已有模型导出工具；独立 AI1 分支已完成 C++ 推理基础层与模型管理，尚未合入主工作区 |
| 高级工具、格式兼容与正式发布 | 继续开发和验收 |

主工作区七组本地 C/C++ 回归记录为 **238 项通过、0 失败、2 项可选基准跳过**。这些结果覆盖已实现功能，完整 Mac 功能与视觉一致性仍需验收。

外部 `Compositor-ai1` 子项目的当前 GPU 范围为**受支持的 Intel 核显 DirectML**，其他显卡使用 CPU；NVIDIA CUDA 留待后续。AI 主体/物体选区、边缘细化和去背景交互尚未完成。独立分支的测试结果与本地主工作区进度分别记录在 [开发日志](windows/DEVELOPMENT_LOG.md)。

后续重点是合入 AI1 基础设施，完善高级变换、画布内文字编辑和高级选区，扩大真实文件与硬件测试，再完成跨平台对照和发布验收。详细任务见 [移植进度](windows/PORTING_STATUS.md)。

## 构建与使用

开发环境需要 Visual Studio C++ 工具链、Windows SDK、CMake 和 Qt 6 的 MSVC x64 开发包。本地已验证 Qt 6.10.2。仓库根目录运行：

```powershell
.\windows\build.ps1 -QtRoot C:\Qt\6.10.2\msvc2022_64
```

该命令构建并运行测试；依赖准备、可选打包和运行说明见 [Windows README](windows/README.md)。便携包需完整解压，保留程序旁的 DLL 与插件目录，再运行 `Compositor.exe`。目前仍为开发预览，不代表正式发布已完成。

## 文档

- [开发日志：2026-10-03，含外部 AI1 子项目](windows/DEVELOPMENT_LOG.md)
- [版本变更记录](windows/CHANGELOG.md)
- [移植进度与待办](windows/PORTING_STATUS.md)
- [L1–L4 交付记录](windows/L1-L4_REPORT.md) · [C1–C5 交付记录](windows/C1-C5_REPORT.md)
- [Windows 架构](windows/ARCHITECTURE.md) · [项目格式](docs/project-format.md)
- [第三方许可说明](windows/THIRD_PARTY_NOTICES.md) · [MIT 许可证](LICENSE)
