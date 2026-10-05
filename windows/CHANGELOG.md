# Windows preview changes

## Unreleased

### 2026-10-05 — IO1–IO5, version remains 0.4.0

- Added debounced external project/asset refresh with incomplete-write protection, dirty-tab confirmation and fingerprint checks around background saves.
- Added background recovery snapshots, per-process recovery locks, startup recovery as unsaved copies, safe old staging cleanup and 20 recent files.
- Added a configurable history memory budget that counts shared images once and evicts old commands without changing current content.
- Added native WIC HEIC/HEIF import with Unicode paths, orientation, profile conversion and missing-codec diagnostics.
- Added PSD/PSB Bezier masks and six native layer-effect conversions, grayscale import, a fourteenth regression suite and seven pinned real-format fixtures. Advanced Photoshop effects and complete visual parity remain limited; see [IO1–IO5 verification](IO1-IO5_REPORT.md).
- Acceptance fixes: staging cleanup no longer deletes a stage whose same-suffix backup is still present (a save interrupted between its two renames, where the stage is the latest copy); asset rehashing backs off with hashing time instead of running every three seconds; oversized assets are named in the save error; Photoshop paths are limited to 1,000 shape operations; present but unusable fixture references now fail the real-file check. Rotated HEIC orientation remains a known issue.
- Kept `.comp` format 11; no saved fields, updater or new release package.

### 2026-10-04 — Unicode model paths on CI, version remains 0.4.0

- Reproduced the ONNX Runtime initialization crash with an emoji model filename on a legacy Chinese ANSI code page; the English GitHub runner cannot represent the original Chinese fixture either.
- Embedded a UTF-8 process manifest in the application, AI probe and AI-linked tests. Wide model paths remain intact; model loading and profiling no longer depend on the legacy system code page. Windows 10 version 1903 or later is required for this setting.
- Added process-code-page and Unicode profiling regressions, and extended the model fixture to a Chinese/Japanese/emoji directory and filename. Full Release build and 13/13 CTest suites pass; the default QtTest total increases to 386 with 20 opt-in skips.

### 2026-10-04 — combined acceptance, version remains 0.4.0

- Verified today's AI1–AI3, T1/T2, S1/S3, P1–P3 and X1/X2 branches are integrated with the existing L/R/C work; see [combined acceptance](ACCEPTANCE_REPORT_2026-10-04.md).
- Deployed Qt's native Schannel TLS plugin beside AI application/test targets; the explicit pinned HTTPS transfer now succeeds without OpenSSL on PATH.
- Corrected transform screenshot checks to use physical pixels at the captured device pixel ratio; native Windows display scaling and offscreen checks remain supported.
- Kept push CI to build and test without packaging or uploading a portable ZIP, matching the current source-only delivery request.
- Updated current README/status notes while preserving historical development-stage evidence. No application or project-format version change, updater, model upload or new release package.

### 2026-10-04 — AI3, version remains 0.4.0

- Added soft BiRefNet Lite background removal on the active pixel layer, preserving original pixels in an editable layer mask.
- Added guided refinement, contrast and edge shift, cached inference, cancellable checkerboard/mask previews and full-resolution commit with one undo step.
- Preserved selection and existing-mask coverage; baked old placement into the layer grid and linked the resulting mask. Undo restores original mask metadata.
- Made the AI2 failure rollback test await the completed error state instead of a fixed sleep, and made its fixture failure flag atomic.
- Added an eleventh CTest suite, independent Python reference generation, CPU/Intel real-editor checks and Chinese/Japanese UI. See [AI3 verification](AI3_README.md).

### 2026-10-04 — AI2, version remains 0.4.0

- Added BiRefNet Lite subject selection and SAM 2/MobileSAM foreground/background point selection on the canvas.
- Added background inference, encoding reuse, transactional previews and cancel/apply/undo; prevented stale output after image, tool, selection or project changes.
- Normalized LF/CRLF when comparing complete model license texts; model size/SHA256 and license-content checks remain strict.
- Added `compositor_ai_selection_tests` and included it in future offline self-test bundles. Ten CTest suites and CPU/Intel real-model/editor checks pass; see [AI2 verification](AI2_README.md).

### 2026-10-04 — follow-ups

- Translated 30 more edit-history labels and messages (brush, clone, healing, import, layer and selection operations) for Simplified Chinese and Japanese, so Edit > Undo/Redo no longer shows them in English.
- Fixed an intermittent crash when closing a window with unsaved edits: a destroyed page no longer notifies the window that is tearing it down.

### 2026-10-04 — S1/S3, version remains 0.4.0

- Added polygonal lasso with vertex/closing previews, Backspace, apply/cancel and selection combination modes.
- Added cancellable background circular expansion/contraction, preserving feathered coverage and explicit empty selections.
- Added selection-to-mask for layers/folders in canvas coordinates, with undo and project persistence.
- Added Color Range with replace/add/exclude samples, fuzziness, inversion, background mask/live canvas preview, transactional OK/Cancel and Chinese/Japanese translations.
- Extended canvas interaction and persistence regression on the AI1 integration baseline; nine CTest suites pass. See [verification](S1-S3_REPORT.md). S2 and AI editing remain separate tasks.

### 2026-10-04 — X1/X2, version remains 0.4.0

- Added native canvas text input with IME composition, UTF-16 rich font/color ranges, transformed paragraph-frame move/resize/reflow, Ctrl+Enter Apply and Escape cancellation. Pending text commits on tool/layer/tab changes, other edits, Save and Export; each edit is one document undo step.
- Added an inline typography toolbar and persistent Windows font mappings. Automatic substitutions cover common Mac/PostScript names; document font names and unedited PNG fallbacks remain intact.
- Basic shape layer and group/numeric resizing now redraws rectangle/rounded-rectangle/ellipse/line sources at the new dimensions, retaining corner radius, stroke width and mask placement.
- Added 22 text/shape regression cases (24 QtTest checks pass in offscreen and Windows modes); all eleven Release CTest suites pass. Extended self-test delivery to include transform and text/shape suites. See [X1/X2 delivery notes](X1-X2_REPORT.md). No release ZIP was generated.

### 2026-10-04 — P1–P3, version remains 0.4.0

- Completed growing paint/mask surfaces with preserved affine placement, implicit mask coverage, stroke opacity, selection clipping, undo and cancellation. Materialized small masks into editable grids.
- Completed release-endpoint interpolation for dab tools and closer spacing for fine/long brush segments; verified existing smoothing and Shift lines.
- Fixed clone sampling during growth, negative source coordinates, transparent source-over blending and canceled alignment. Gradients now use document coordinates and, like foreground/background fills, grow to the canvas or selection; gradients also target masks.
- Added 13 canvas regressions (33 canvas tests pass); all ten Release CTest suites pass. See [P1–P3 delivery notes](P1-P3_REPORT.md). No portable ZIP or release was generated.

### 2026-10-04 — AI1 integration, version remains 0.4.0

- Merged AI1's C++ inference/model management with the existing C1–C5 editor, preserving canvas interactions, customizable shortcuts and Chinese/Japanese catalogs.
- Unified nine CTest suites, including AI/model and canvas tests; deployed ONNX Runtime/DirectML/Qt Network beside the canvas executable as well as other UI consumers.
- Added an integration regression for both AI-model and keyboard-shortcut menu registration under Chinese/Japanese translation. Extended offline self-test delivery to include the canvas suite.
- Retained the Intel-integrated-only DirectML scope, CPU fallback and unpublished model endpoints. AI editing interactions remain pending; model weights/cache/artifacts remain outside Git.
- Updated the README and migration status; preserved the previous AI1 package/report as historical evidence. See [integration verification](AI1_INTEGRATION_REPORT.md). No complete ZIP was regenerated or published.

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
