# C1–C5 delivery notes

Application version remains **0.4.0**; `.comp` remains **11**. C/C++ source remains MIT licensed. Automatic updating stays excluded. This step completes the current Windows canvas/tool scope in `Windows开发计划-1.0.docx`; later transform, text, selection and AI work remains on the migration checklist.

| Task | Delivered |
| --- | --- |
| C1 | Enter/Escape apply/cancel; remappable hold-to-pan; brush size/hardness, one/two-digit opacity, blend-mode cycling, colors, tool modes, zoom, layer/selection/pixel nudging and missing supported menu keys |
| C2 | Brush diameter/hardness outlines, clone source crosshair, point/average color sampling ring, cached animated selection boundaries, single-layer/mask resize and rotation handles with Shift/Alt/Ctrl behavior |
| C3 | Rulers; guide creation, dragging, removal, locking, clearing, history and project save/load; configurable grid spacing/subdivisions/colors; shared screen-tolerance snapping for move/resize/crop/shapes/guides |
| C4 | Context-sensitive controls and project-session values; brush/blur/shape/marquee modes, smoothing/straight strokes, clone sampling/alignment, all three C healing modes, wand sample modes, selection combination/anti-alias, gradient type/endpoints/reverse/opacity, shape radius/line width, text font/size/alignment/tracking/leading and zoom |
| C5 | Stable menu/tool/canvas command registry, searchable shortcut dialog, single-chord and conflict validation, default restoration, remembered overrides, native text-input protection and physical release tracking for the temporary Hand tool |

Guides reuse the existing manifest field. View preferences and shortcut overrides live in QSettings; tool settings remain in EditorSession. Selection/view changes do not dirty image content. Transform gestures preserve source raster pixels and editable layer metadata; cancel/undo restores the previous document. Mask gestures preserve the layer transform. Text is still created/edited through dialogs; its direct canvas editing is T2 work.

Default Windows keys keep `Ctrl+I` for image import; Invert therefore uses `Ctrl+Alt+Shift+I`. `Delete` deletes a layer when Move/Select has no pixel selection, and clears a pixel selection otherwise. **Edit > Clear Pixels** still clears a whole image/mask explicitly. **Edit > Keyboard Shortcuts** remaps registered commands; commands for pending tools will register when those tools are implemented.

## Validation

MSVC Release build and seven local Qt suites: **238 passed, 0 failed**, with **2 optional benchmarks skipped** (core 107, UI 42, Photoshop 18, RAW 6, Dither 17, layers 28, canvas 20). The new suite exercises apply/cancel/undo, opacity/brush keys, selected-pixel movement, guide drag/save/lock/delete, zoom-independent snapping, resize/rotation/mask handles, smoothing and final stroke endpoints, clone alignment/merged sampling, wand sampling, healing modes, contextual options/tab state, fill/delete/select behavior, remapping/conflicts/hold release, native typing and view-only overlays. Existing rendering checks compare the underlying pixels with transient overlays disabled; overlay tests separately verify animation and unchanged document content.

Chinese/Japanese menus, tool options and shortcut dialog were inspected through local screenshots; grid and guide dialog cancellation retains preferences. `git diff --check` passes. Test dependencies, including Qt/LibRaw DLLs and the offscreen plugin, are deployed beside the new test executable by CMake. No release application EXE or ZIP was generated for this step. Local QA files remain under ignored `artifacts/`; this step has not been pushed.

Passing local tests does not establish Mac reference-image parity, target-hardware large-document performance, or clean-machine distribution acceptance. Multi-layer/folder/mesh transforms, polygon/object/subject selection, inline text editing, remaining format compatibility and AI runtime integration are separate work packages.

The original local Word plan was saved with C1–C5 progress and verified for ZIP/XML integrity, table structure, status cells and regression counts. Word PDF export did not finish across isolated attempts, so its updated pagination has not been visually verified. Private export helpers were stopped; the user’s Word instance was preserved.
