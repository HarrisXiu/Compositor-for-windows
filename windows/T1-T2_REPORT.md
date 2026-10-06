# T1–T2 delivery notes

Application version remains **0.4.0**; `.comp` remains **11** (distortion resamples pixels and leaves ordinary affine transforms, so nothing new is saved). C/C++ source remains MIT licensed. Automatic updating stays excluded.

| Task | Delivered |
| --- | --- |
| T1 | Ctrl-drag any transform handle to distort: a corner handle moves its corner, an edge handle both corners of that edge, Shift holds an axis. The preview resamples the layer while dragging; letting go resamples at full size, trims the layer to what is visible and records one undo step. A convex shape is warped in perspective; a corner pulled past its neighbors is warped as two triangles; a collapsed shape is ignored and the last usable one stays. A linked mask goes with the pixels (a mask placed apart is carried over its own bounds, an unlinked one keeps its place); editable text/shape sources are rasterized. A handle released where it was grabbed records nothing; Escape restores the document. |
| T2 | Selected layers, and the contents of selected folders, share one box with the same handles: resize (Shift/Alt), rotate (Shift snaps to 15°) and distort, each member carried by the box's transform, a linked placed mask following. The Move tool drags every selected layer, a folder with its contents, or a mask alone, snapping the group's bounds. Alt-drag with the Move tool duplicates the selected roots with their descendants, leaves the originals and drags the copies (one undo step; Escape removes them). Auto-Select (an existing option) still picks the topmost layer under the pointer. |

New code: `distort.cpp/.h` (distortion, corner and transform algebra), `canvas_transform.cpp` (the subject, handle, group and Move-tool gestures). `canvas_interaction.cpp` and `canvas_tools.cpp` now call into it, replacing the single-layer code; resizing a single layer now carries a linked placed mask.

## Differences from the Mac app

- ~~A distortion is applied when the handle is released~~: since `feature/parity-gaps` it waits until applied, as on the Mac (see [parity follow-up](PARITY_GAPS_REPORT.md)). Distorting an unlinked mask alone is not implemented; the Mac does not support it either.
- Members of a folder transform even when hidden; the box is drawn around the visible ones. Adjustment layers are not transformed by the handles (the Move tool and nudge still move them).
- Ctrl on a handle now distorts; it no longer bypasses snapping during a resize (Move-tool dragging still does).

## Validation

MSVC Release, eight suites: **258 passed, 0 failed**, 2 optional benchmarks skipped (core 107, UI 42, Photoshop 18, RAW 6, Dither 17, layers 28, canvas 20, transform 20). The transform suite covers corner and box algebra, member following (scale, plain move, quarter turn), identity distortion of a rotated flipped layer, pixels on the distorted shape, folded and collapsed shapes, mask distortion, Ctrl-drag distortion as one undo step with cancel and no-move cases, group scale/rotate/distort/folder transforms, multi-layer moves, Alt-drag duplication with undo and Escape, and linked placed masks following a resize. The distorted group overlay was inspected in a screenshot. Passing local tests does not establish Mac reference-image parity or target-hardware large-document performance.
