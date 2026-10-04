# Windows preview changes

## Unreleased

### 2026-10-04 — T1/T2, version remains 0.4.0

- Added Ctrl-drag free distortion of a layer or a group: the box's corners move freely and the pixels (and a linked mask) are resampled in perspective, or as two triangles for a folded shape, as one undo step.
- Selected layers and the contents of selected folders now scale, rotate and distort together in one box; the Move tool drags every selected layer and a folder's contents; Alt-drag duplicates the selection and drags the copies. Linked masks placed apart follow resizing.
- Added `compositor_transform_tests` (20 tests). Combined Release regression: eight suites, 258 passed, 0 failed, 2 optional benchmarks skipped.

### 2026-10-03 — version remains 0.4.0

- Completed the L1–L4 layer/canvas scope: thumbnails, multi-selection, merge/group/order operations, cross-project subtree copy, editable crop, canvas/image size, trim and flips.
- Deployed Qt/LibRaw DLLs and the offscreen plugin beside test executables, fixing the missing Qt6Gui.dll test-launch error.
- Integrated R2–R4 with the existing layer/canvas editing behavior; the rendering changes are listed below.
- Completed C1–C5 for current Windows tools: canvas keys and overlays, single-layer/mask transform handles, rulers/guides/grids/shared snapping, contextual tool options and persistent configurable shortcuts. Extended Chinese/Japanese UI coverage.
- Main-worktree Release regression records 238 passed, 0 failed and 2 optional benchmarks skipped across seven suites. This does not establish Mac parity or target-hardware performance acceptance.
- Added pinned FP32 model export and verification tooling for BiRefNet, SAM 2 and MobileSAM, with model hashes, signatures and original licenses. Python is development tooling only.
- Recorded external AI1 progress separately: C++ inference/model management, supported Intel integrated GPU DirectML and CPU fallback, plus complete offline test delivery. AI1 remains on its own branch; application AI selection/refinement/background-removal interactions and main-worktree integration are pending.
- Rewrote the root README as a Windows project overview and added the [daily development log](DEVELOPMENT_LOG.md), including external AI1 evidence and integration boundaries. Models, caches and artifacts remain excluded from Git; no version/schema bump or updater was added.

### Integrated rendering changes

- Replaced the 1,600-pixel whole-document canvas preview with 256-pixel tiles rendered for what is on screen, at full resolution when zoomed in and at power-of-two reductions when zoomed out; tiles render in parallel and are cached.
- Refreshes redraw only the tiles over layers that changed; recording a finished edit redraws nothing. Brush dabs redraw only the area they touch, compositing the edited layer and those above over a kept backdrop.
- Zoomed-out views halve layer images before resampling them, so they stay sharp; exports are unchanged. Painting updates only the changed part of the halved images and of a mask's coverage image.
- Canvas tiles render in the background from a copy of the document, so opening or zooming a large document doesn't block the window; until a tile is ready, other zoom levels stand in for it. Edits in progress still redraw at once.
- Painting a layer with effects redraws them only within their reach of the brush, exactly as a full rebuild would, and redraws every tile that reach touches. The effects cache now holds a large layer's effects (up to 512 MiB).
- The eyedropper samples the full-size composite; the magic wand reuses one until the document changes. A pixel grid appears at 800% and above.
- Add Noise and Grain adjustments take the position of the area being rendered, so parts of an image match the whole.

## 0.4

- Install Qt packages sequentially with the Windows runner's external 7-Zip to avoid shared-directory extraction races and intermittent py7zr Bad7zFile failures; stop immediately if the installer dependency setup fails.
- Split the editor into window, page/history, menus, layer panel, tool options and dialog modules; introduced concrete canvas tools with common event/overlay/cancellation hooks.
- Moved selection, foreground/background colors, editing target and tool options into per-project session state. Added color swap/reset and Escape/temporary Space panning.
- Added selection undo/redo for marquee/ellipse/lasso/wand, all/deselect/inverse and feather; crop/resize history restores the original selection.
- Separated saved content identity from selection history so selection edits do not cause unsaved markers or close prompts, including background-save interactions.
- Replaced blend-name comparisons inside pixel loops with enum dispatch; added a Normal path and row-parallel compositing with frozen-baseline pixel equivalence tests.
- Kept .comp schema version 11, project MIT licensing and exclusion of automatic updates. Full parity and viewport/tile rendering remain incomplete.

## 0.3

- Added native PSD/PSB import, layer/folder/mask/clipping conversion, supported adjustment conversion, editable horizontal type and basic live shapes, and conversion reports.
- Added LibRaw camera RAW development with a serialized preview per dialog, cancellation, full-size import, controls and reset.
- Ported all Dither pixel kernels to C++ parallel execution, and added palettes, chunky pixels, ASCII and CRT controls.
- Added Blur, Smudge and Liquify brushes, including mask blur, selection clipping and stroke undo/cancellation.
- Added Vignette, Bloom / Glow and Tonal Contrast; expanded Camera Raw filter controls to Effects, Detail, Optics and Calibration.
- Added Camera Raw parametric/point curves, Mixer/Point Color/Grading/Geometry pages, sampling and guided correction, asynchronous Auto WB, scopes and preview-only clipping/masking indicators.
- Added live Simplified Chinese, English and Japanese UI selection with saved preferences and stable project parameter values.
- Corrected Lens Correction strength to the upstream 0.35 scale; kept unlinked Photoshop masks at an independent placement.
- Added PSD/PSB, RAW and Dither suites and additional pixel/UI regression coverage. Five suites run locally and in the Windows build workflow.
- Kept the root MIT license unchanged. Automatic updating remains excluded. Bundled LibRaw source/licenses are included separately with its DLL.

Full feature parity remains tracked in PORTING_STATUS.md.

## 0.2

- Added all six layer effects, all twelve adjustment kinds, per-channel Curves/Levels, Hue/Saturation color ranges, editable text/shapes, and clipping/folder rendering fixes.

## 0.1

- Established the C11/C++20 Windows application, project IO, CPU compositing, layers, selections, painting, original C filter adapters, tests, build workflow and portable packaging.
