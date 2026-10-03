# L1–L4 delivery notes

Application version remains **0.4.0**; `.comp` remains **11**. Source remains MIT. Automatic updating is excluded. This step implements the layer/canvas lane of `Windows开发计划-1.0.docx`; the Windows application still has other migration work pending.

| Task | Delivered |
| --- | --- |
| L1 | Layer/mask thumbnails and separate edit targets; Ctrl/Shift multiselection; context menu; Ctrl-thumbnail alpha/coverage selection; session folder expansion and scroll preservation |
| L2 | Ctrl+E merge down / selected layers / folder; grouping and ungrouping; selected sibling block raise/lower; move out of folder; document-driven drag nesting/reordering with cycle validation |
| L3 | Full subtree copy/cut/paste and cross-tab drag copying; new UUIDs and asset names; internal clipping/parent remapping; external clipping baked when needed; immutable clipboard snapshots; source project preserved |
| L4 | Crop create/move/eight-handle resize, free/preset ratios, Shift/Alt/Ctrl behavior, edge/guide snapping, overlay and explicit apply/cancel; nine-anchor Canvas Size with relative/physical units and extension fill; Image Size/resampling/DPI; transparent/corner-color Trim; whole-canvas horizontal/vertical flip |

Every destructive command is one document undo step. Undo restores pixel selections and selected layer IDs. Canvas offsets and flips also transform independent masks and guides. Crop drafts do not dirty the project or create history entries. Canceling a size dialog leaves the document unchanged. Image resampling rasterizes editable text/shapes and independently rotated content; DPI-only changes retain asset pixels.

## Validation

The `compositor_layers` suite covers merging/opacity/clipping, ancestor selection, transparent content, grouping/order, subtree save/load, cross-project transfer, source deletion/closure, independent masks, thumbnail targets, reparent cycle rejection, size anchors/extension/guides, nonuniform rotated resampling, trim, flips, rollback, UI history, cross-tab drop, crop gestures/cancellation, units/DPI and Chinese/Japanese action labels. Existing core, UI, PSD, RAW and Dither suites are also run.

Final MSVC Release verification: **192 passed, 0 failed**, across six suites (core 93, UI 30, PSD 18, RAW 6, Dither 17, layers/canvas 28). The optional blend timing benchmark is the single expected skipped test. `git diff --check` also passes. These results include the fix for a tree-item lifetime crash when changing layer visibility and rotated uniform independent-mask coverage during resampling.

CMake deploys required Qt and LibRaw DLLs plus the offscreen plugin beside Windows test executables. This fixes the missing `Qt6Gui.dll` test-launch error. Verification uses offscreen tests, with Windows system-error dialogs suppressed in the local test launcher.

## Integration limits

- This step creates test executables only; it does not regenerate the release application or ZIP. This report covers the L1–L4 source and documentation update; artifacts remain excluded from Git.
- Cross-project clipboard data is available inside the running Compositor process. External applications still use image paste.
- Shared transform snapping, rulers/grid editing, multi-layer/folder transforms and Alt-drag duplication belong to other plan tasks. Crop currently owns its edge/guide snapping logic.
- CPU previews remain limited to a 1,600-pixel long side. Interactive crop rendering has not been accepted against the plan's large-document performance target.
- Mac reference images, real Windows mouse-drag acceptance and broader project interoperability remain part of integration acceptance. Automated results do not establish full Mac parity.
- `artifacts/` remains ignored and contains local validation logs/helpers only.
