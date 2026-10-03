# Compositor

## Windows C/C++ preview

**当前 Windows 版尚未完全完成，后续将持续更新。** 目前为可运行预览版，仍有渲染性能、高级画布交互、AI 工具和格式兼容工作待完善；请以 [移植进度](windows/PORTING_STATUS.md) 为准。

This fork includes a Windows migration preview built with C++20, Qt 6 and the original C pixel algorithms. See [Windows build, usage and migration status](windows/README.md). The Windows preview is still being developed and does not yet have full feature parity with the macOS application described below.

The project and Windows port remain MIT licensed. Automatic updating is excluded from the Windows port; all other original features remain in its migration scope. Qt, LibRaw and Microsoft runtime components retain their respective third-party licenses.

### 移植计划与当前进度

Windows 版使用 C/C++，当前版本保持 **0.4.0**，支持简体中文、英语和日语。移植按“基础能力 → 渲染性能 → 功能与交互完善 → 兼容性及发布验收”推进，目标是逐步对齐原版；自动更新不在移植范围内。

| 方向 | 当前进度 |
| --- | --- |
| 项目读写、编辑器框架与基础功能 | 主要完成 |
| 图层管理、跨项目复制、裁剪和尺寸调整 | L1–L4 主要完成，已通过本地回归 |
| 大图渲染与绘画性能 | R2–R4 已与 L1–L4 合并，通过合并后的本地回归；大文档与跨平台验收待完成 |
| 高级交互、格式兼容与 AI 工具 | AI1 C++ 推理与模型管理已接入；仅支持 Intel 核显 DirectML，其他显卡使用 CPU；CUDA 与 AI 选区/去背景应用工具待完成 |
| 跨平台一致性、兼容性与正式发布 | 待完整验收 |

R2–R4 分块渲染与 L1–L4 图层/画布工作已整合，下一阶段进行大文档与跨平台验收，并逐步补齐交互和剩余功能。合并后的六组本地回归共 **218 项通过、0 失败**，另有 2 项可选基准默认跳过；这不代表真实相机、Photoshop 样例或与 Mac 版参考图的一致性验收。后续将持续更新，概况见 [移植进度](windows/PORTING_STATUS.md)。

### Windows AI1 当前状态

AI1 的推理基础层和模型管理已接入。当前 GPU 加速只支持 **Intel 核显 DirectML**；同机有 NVIDIA 独显时仍优先选择受支持的 Intel 核显，没有可用 Intel 核显时使用 CPU。NVIDIA CUDA 后续单独开发，目前未实现；NVIDIA/AMD DirectML 不再属于适配和验收范围。

Release 构建和 8 组回归通过；四种真实模型的 Intel GPU 检查 **150/150** 通过，选中 NVIDIA 后的 CPU 回退检查 **132/132** 通过。AI 主体/物体选区、边缘细化和去背景的应用交互仍待实现，Windows 版尚未完成完整功能对齐。

跨机器测试使用完整 `AI1-complete-self-test.zip`：**完整解压 → 双击“开始测试.cmd” → 带回生成的 results ZIP**。无需安装 Python、Qt 或开发工具。模型与验证日志保存在本地，不创建远端 Release、不上传资产。

使用范围、模型、测试包位置和后续工作见 [AI1 README](windows/AI1_README.md)；构建说明见 [Windows README](windows/README.md)，验证证据见 [AI1 验收报告](windows/AI1_REPORT.md)。

Adobe Photoshop costs too much and tools like GIMP don’t feel familiar enough for me to stay in flow. That’s why I built Compositor.

The goal was to create a full-featured image editor that is completely free and open source. I used to use Photoshop for compositing and post-processing, so Compositor is built around that workflow - with the tools needed to create a pixel-perfect final image.

Because it’s open source, you can download the Xcode project and add, remove, or modify any feature to fit your workflow.

## Installation

### Download
Get Compositor from [robbietilton.com/compositor](https://robbietilton.com/compositor), or download the latest release directly from [GitHub Releases](https://github.com/robbietilton/Compositor/releases/latest).

### Homebrew

```sh
brew install --cask robbietilton-compositor
```

## Features

### Layers
- Layers and folders, with opacity and Photoshop's full set of blend modes in its order — a folder's opacity dims everything inside it
- Layer masks: paint, fill, invert, blur and feather them anywhere on the canvas, past the layer's own pixels; link or unlink them to transform a mask on its own
- Clipping masks and folder masks
- Adjustment layers: Hue/Saturation, Levels, Curves, Exposure, Gradient Map, Grain, Black & White, Color Balance, Invert, Gaussian Blur, Motion Blur and Noise
- Layer effects: Stroke, Drop Shadow, Color Overlay, Inner Shadow, Outer Glow and Inner Glow, rendered on the GPU and editable at any time
- Merge Down, Merge Layers and Merge Group (⌘E)
- Duplicate, rename inline, reorder and nest by drag and drop; Option-drag to duplicate; a right-click menu in the Layers panel
- Copy and paste whole layers and folders (⌘C/⌘V with no selection), within a project or between projects, or drag them between projects

### Transform
- Non-destructive move, scale, rotate and flip — images keep their full resolution however small you make them
- Free distort (⌘-drag a handle), with Shift to lock to an axis
- Transform several layers, or a whole folder, together
- Snapping to canvas and layer edges and centers, with guides
- Exact values for position, size, scale and angle, stepped with the arrow keys
- Flip Layer and Flip Canvas, horizontal and vertical

### Selections
- Rectangle and Ellipse Marquee, Freehand and Polygonal Lasso, and the Magic tool — Wand selects by color, Object traces whatever you click (Tab switches)
- Select Subject, and Expand, Contract and Feather on any selection
- Add to and subtract from selections, move the outline, or move and duplicate the pixels inside
- Load a layer's pixels or a mask as a selection
- Content-Aware Fill, which can also extend an image past its edges

### Painting and retouching
- Brush with size, hardness, opacity and smoothing, in Paint or Erase mode (B and E), and Shift for straight lines
- Spot Healing Brush (content-aware)
- Clone Stamp, aligned or not, sampling one layer or all of them
- Blur tool, on pixels or masks
- Gradient tool and Shape tool (rectangles, rounded rectangles, ellipses and lines), which stay editable rather than being rasterized
- Type tool (T): inline multiline editing in draggable, resizable paragraph boxes; font, size, color, alignment and spacing in the tool header; transform text and use it as a clipping mask
- Eyedropper and a full color picker

### Adjustments and filters
- Camera Raw filter: light, color, curves, color mixer, color grading, detail, optics and geometry, in a panel beside the canvas
- Levels (with Auto), Curves, Hue/Saturation, Exposure, Gradient Map, Grain, Black & White, Color Balance and Invert
- Gaussian Blur and Motion Blur that spread past a layer's edges
- Add Noise, Vignette, Bloom / Glow, Tonal Contrast, Lens Correction and Remove Background
- Live previews, limited to the selection when there is one

### Canvas and files
- Multiple projects in tabs
- Rulers (⌘R), guides dragged from them, a layout grid with adjustable spacing and subdivisions, and Snap To for guides, grid, layers and document bounds
- Crop with snapping, ratios including 3:4 and 9:16, and Option for symmetric cropping; with a selection, the crop starts at it
- Canvas Size, Image Size and Trim
- Sharp high-quality downsampling when zoomed out, and a pixel grid when zoomed in
- Import JPEG, PNG, HEIC, TIFF, SVG, camera RAW (with a develop step first) and Photoshop PSD and PSB (8-bit RGB; not CMYK). Photoshop folders, masks, blend modes, fill rectangles/ellipses, and simple horizontal text stay editable; other vectors and vertical text become pixels. A conversion report is shown before anything is applied.
- Large documents: the memory budget scales with your Mac, and a Photoshop file too big to open has its layers cropped to the canvas instead
- Export JPEG with a live preview (⇧⌥⌘S); Copy Merged
- Keep working while a project saves
- Photoshop-style keyboard shortcuts throughout, remappable in Edit > Keyboard Shortcuts
- Drag a number's label to scrub its value, as in Photoshop
- Automatic updates, signed and notarized

### Works with AI agents
- AI agents and scripts can build and edit projects directly: a `.comp` is a folder of PNG layers and a manifest, and an open project updates live as it's written. See [Writing Compositor projects](docs/writing-comp-files.md)

## Requirements

- macOS 26.0 or later on a Mac with Apple silicon
- Xcode 26 or later (to build from source)

## Building

Open `Compositor.xcodeproj` and run the **Compositor** scheme.

## Releasing

`scripts/release.sh` builds a Release version, signs it with Developer ID, notarizes and staples it, and packages it into `dist/Compositor-<version>.dmg`.

It needs, all kept outside this repository:

- a **Developer ID Application** certificate in the login keychain
- notarization credentials saved with `xcrun notarytool store-credentials "compositor-notary" …`
- [`create-dmg`](https://github.com/create-dmg/create-dmg) (`brew install create-dmg`)

## License

MIT — see [LICENSE](LICENSE).
