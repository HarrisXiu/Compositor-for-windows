# Windows migration status

Target: preserve the original Compositor workflow and project format in a C11/C++20 Windows application. Project source remains MIT licensed. Automatic updating is explicitly excluded. This checklist tracks delivery, not a claim of full parity.

| Area | Current state | Remaining work |
| --- | --- | --- |
| Native app / project IO | Tabs, import, .comp versions 1–11, background save, safe replacement, Unicode CLI paths | Recent files, recovery/autosave, external changes, shell associations |
| Layers | Layer/mask thumbnails, multi-selection, context menu, Ctrl-thumbnail selection, three Ctrl+E merge modes, grouping/ungrouping, sibling moves, subtree copy/paste and cross-tab copy, opacity, 24 blend modes, masks, clipping | Multi-layer/folder transforms, Alt-drag duplication, complete Mac reference comparison |
| Canvas size / crop | Editable crop frame with ratios, center resize and edge/guide snapping; nine-anchor Canvas Size, relative/physical units, extension colors, Image Size/DPI/resampling, transparent/corner-color Trim, whole-canvas flips; selection-aware undo | Shared snapping/grid service and large-document interactive performance |
| Painting | Brush/eraser, mask paint, clone, spot heal, content-aware fill, Blur/Smudge/Liquify, folder-mask blur, cancel/undo | Brush dynamics/smoothing, aligned/sample modes, stroke expansion beyond asset bounds, history memory budget |
| Selections | Rectangle/ellipse/lasso/wand, add/subtract/invert/feather, shape outlines | Polygon/object/subject tools, refine edge, selection transforms |
| Text / shapes | Editable source metadata, rich font/color runs, paragraph controls, rounded rectangles, ellipses, lines | Direct canvas editing/resize, exact typography/font matching, vector resize behavior |
| Adjustments | All 12 kinds create/edit/render; per-channel Levels/Curves; color-range Hue/Saturation | Complete original sliders/sampling panels, spatial extents and document-coordinate noise parity |
| Layer effects | All six types, mask-aware source rendering, transforms, enable/remove, undo | Mac image comparison, full canvas live previews and large-document acceleration |
| Filters | Existing C algorithms, Gaussian/Motion Blur, all 11 Dither styles, Vignette, Bloom / Glow, Tonal Contrast; Camera Raw Light/Color/Curve/Mixer/Point Color/Grading/Effects/Detail/Optics/Geometry/Calibration, Auto WB, sampling/guides/scopes/indicators, modal previews | Camera Raw targeted gestures/RGB readout, mesh distort, spatial bounds and Mac reference parity |
| Languages | Live Simplified Chinese, English and Japanese; remembered choice, canonical parameter values and document content preserved | Remaining decoder/conversion diagnostics; native system-dialog language follows Windows |
| View / layout | Pan/zoom/fit, imported guides | Rulers, guide editing, snapping, grids, transform handles |
| File formats | Qt image import/export, native PSD/PSB raw/RLE 8-bit RGB layers/folders/masks/clipping/adjustments, supported editable type and vogk basic shapes, LibRaw RAW development | Complete vector/remaining PSD conversion, broader real camera/Photoshop fixtures, HEIC, remaining original import/export behavior |
| AI tools | Not yet implemented | Windows subject/object selection, matte refinement, background removal |
| Performance | R1 complete in this checkout. R2–R4 implemented in the separate `r2-r4-tiled-rendering` worktree: viewport/256-pixel tiles, incremental painting, reduced images, full-resolution sampling and pixel grid; five CTest suites passed there | Integrate that work with L1–L4 and rerun combined acceptance; optional GPU backend and large-canvas/history budgets |
| Distribution | MSVC/Qt build, test suites, portable package, CI workflow | Windows installer, icons/resources, shell integration, clean-machine verification |
| Automatic update | Excluded by user request | None; do not add an updater or update checker |

Six regression suites cover project integrity, blending/masks/clipping, adjustments/effects, actual UI edits with undo/cancel, independently encoded PSD/PSB files, synthetic Bayer DNG development, Dither output properties, and the L1–L4 layer/canvas operations. Passing them does not replace real-camera interoperability and reference-image comparisons with the Mac version. Qt remains a dynamically linked LGPLv3 dependency; LibRaw uses CDDL 1.0 with its SDK/source archive included. See THIRD_PARTY_NOTICES.md.

The L1–L4 step keeps application version 0.4.0 and `.comp` version 11 unchanged. See [delivery notes](L1-L4_REPORT.md). No release application or ZIP was generated during this step. CMake now places the Qt/LibRaw dependencies and offscreen plugin beside Windows test executables, so tests do not depend on a manually edited PATH.

The 0.4 improvement step implements the local architecture/session/history part of stage 0: see [ARCHITECTURE.md](ARCHITECTURE.md). Selection changes now support undo/redo without dirtying saved image content; crop/resize restores the selection with the document. Foreground/background colors, target and tool options are independent per project. Blend parsing and row-parallel compositing implement the first optimization in stage 1. R2–R4 are implemented in the separate worktree `Compositor-r2-r4` on `r2-r4-tiled-rendering`, with local regression and timing coverage. Those uncommitted changes have not been integrated here; this checkout still has the 1,600-pixel preview limit. The next step is integration and combined regression, followed by cross-platform and large-document acceptance.
