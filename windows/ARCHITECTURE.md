# Windows editor architecture

The 0.4 preview separates the application shell, per-project interaction state and tool dispatch. These changes prepare the viewport renderer described in the improvement plan; the full-document 1,600-pixel preview remains in place for this release.

| Module | Responsibility |
| --- | --- |
| `editor.cpp` | Window lifecycle, tabs, project import/save, document operations |
| `editor_page.cpp` | Document/selection history, interactive rollback, content save identity |
| `editor_session.h` | Per-project selection, foreground/background, pixel/mask target, tool and options |
| `editor_menus.cpp` | Menus and action wiring |
| `tool_options.cpp` | Tool choices and options synchronized with the active page |
| `layer_panel.cpp` | Layer tree, ordering and transform controls |
| `layer_operations.cpp` | Subtree selection, merging, grouping, sibling/parent moves and ID-safe copying |
| `editor_layers.cpp` / `layer_transfer.cpp` | Layer commands, clipboard snapshots and cross-tab drops |
| `image_operations.cpp` / `image_dialogs.cpp` | Canvas anchors, image resampling, trim/flip and size dialogs |
| `crop_tool.cpp` | Session crop draft, handles, ratios, snapping and explicit commit/cancel |
| `document_dialogs.cpp` | New document and export dialogs |
| `filter_dialogs.cpp` | Filter/adjustment dialogs and Camera Raw controls |
| `effect_dialogs.cpp` | Layer effect dialogs |
| `editable_dialogs.cpp` | Editable text and shape dialogs |
| `canvas.cpp` | View painting, coordinates, event routing and selection operations |
| `canvas_tools.cpp` | Concrete tools implementing press/move/release/key/overlay/cancel interface |
| `canvas_paint.cpp` | Shared painting, warp and stroke-commit kernels |
| `blend.cpp` | Parsed blend modes, Normal path and independent parallel rows |

## Session and history

`EditorPage` owns `EditorSession`. `Canvas` borrows the page's session; a standalone canvas has a fallback session. Colors, options and edit target belong to the page and do not overwrite another project's values when changing tabs.

Selection edits share the undo stack with document edits, but do not count as unsaved image content. A document command snapshots both the document and its selection, so crop/resize undo restores the original coordinate system and mask together. Selection commands retain masks through Qt's copy-on-write storage and do not recomposite pixels.

Each document command carries a unique content-state identity. Background save captures this identity alongside the immutable document snapshot. When save completes, that snapshot's identity becomes the saved state even if the user has since edited or changed the selection. Undo back to the saved content therefore clears the modified marker. Selection-only changes never introduce an unsaved marker or a close prompt.

Selection and session options are not serialized. Layer multiselection, collapsed folders and the crop draft also belong to the session. Document history restores selected layer IDs along with the document and pixel selection. The existing `.comp` schema stays at version 11; no fields have been added. Undo remains capped at 40 steps; a memory budget is still pending.

Layer transfers keep an immutable copy-on-write document snapshot in an in-process MIME registry. Each paste remaps subtree UUIDs, parents, asset filenames and internal clipping references. External clipping is baked when its source is unavailable in the destination; placed masks move with the transferred content even when unlinked. Clipboard snapshots remain usable after editing or closing the source project. Qt model rows are never removed as a side effect of a cross-tab drop; the target explicitly commits a copy. Tree reorders use document operations rather than serializing a partly moved widget model.

Crop gestures edit only the session frame. Enter, double-click or Apply commits one document operation; Escape cancels. Focus loss restores the frame preceding an unfinished drag. Resizing the canvas shifts every layer, independent mask placement and guide by the same anchor offset. Nonuniform Image Size rasterizes transformed assets into axis-aligned bounds, preserving IDs and hierarchy; text/shape sources become pixels. A resolution-only change retains the original assets. Image and mask budgets are checked before resampling allocations.

## Tools and cancellation

`CanvasTool` defines mouse, keyboard, overlay and cancellation hooks. Every tool has a concrete class; related brushes, shapes and selection tools share implementation. The QWidget event handlers route to this interface rather than branching on every tool.

Escape, tool changes and focus loss cancel an in-progress gesture. Pixel edits roll back to the interaction snapshot; canceled selection gestures leave the prior selection intact. Releasing Space ends temporary panning and returns to the page's selected tool. X swaps foreground/background and D resets them to black/white. Additional keyboard behavior and interactive overlays remain subsequent work.

## Compositing

Layer blend names remain canonical strings in the project format. They are parsed once per compositing call into `BlendMode`; no string comparison occurs inside pixel/channel loops. The Normal path avoids backdrop unpremultiplication and channel dispatch. Images of at least 65,536 pixels use QtConcurrent row workers; smaller surfaces use a scalar loop.

Before scheduling work, the destination detaches once and workers receive disjoint raw row pointers. A retained source image preserves correct behavior when the caller passes the same image as source and destination. Workers do not call mutating QImage methods. Equivalence tests use the frozen 0.3 compositor across all 24 modes, partial/zero/full alpha, multiple opacities and both scheduling paths.

Viewport rendering, tile invalidation, painting stack caches, mipmaps and GPU rendering are not part of this change. The optional `COMPOSITOR_BENCHMARK=1 compositor_tests blendTiming` command compares the old and new compositor on this machine; it is not an end-to-end large-document painting benchmark.
