# Windows migration status

Target: preserve the original Compositor workflow and project format in a C11/C++20 Windows application. Project source remains MIT licensed. Automatic updating is explicitly excluded. This checklist tracks delivery, not a claim of full parity.

| Area | Current state | Remaining work |
| --- | --- | --- |
| Native app / project IO | Tabs, import, .comp versions 1–11, background save, safe replacement, Unicode CLI paths | Recent files, recovery/autosave, external changes, shell associations |
| Layers | Tree, folders, ordering, subtree duplication, opacity, 24 blend modes, masks, clipping | Multiple selection and group transforms, original context-menu parity |
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
| Performance | CPU preview bound to 1,600 px, effect cache bounded to 64 MiB | Direct3D backend, tile caches, cancellable workers, large-canvas/history budgets |
| Distribution | MSVC/Qt build, test suites, portable package, CI workflow | Windows installer, icons/resources, shell integration, clean-machine verification |
| Automatic update | Excluded by user request | None; do not add an updater or update checker |

Five regression suites cover project integrity, blending/masks/clipping, adjustments/effects, actual UI edits with undo/cancel, independently encoded PSD/PSB files, synthetic Bayer DNG development, and Dither output properties. Passing them does not replace real-camera interoperability and reference-image comparisons with the Mac version. Qt remains a dynamically linked LGPLv3 dependency; LibRaw uses CDDL 1.0 with its SDK/source archive included. See THIRD_PARTY_NOTICES.md.

The 0.4 improvement step implements the local architecture/session/history part of stage 0: see [ARCHITECTURE.md](ARCHITECTURE.md). Selection changes now support undo/redo without dirtying saved image content; crop/resize restores the selection with the document. Foreground/background colors, target and tool options are independent per project. Blend parsing and row-parallel compositing implement the first optimization in stage 1. Viewport rendering, tile caches and the large-document painting acceptance criterion are still pending; the 1,600-pixel preview limit remains.
