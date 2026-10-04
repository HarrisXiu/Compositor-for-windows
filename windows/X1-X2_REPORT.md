# X1/X2 delivery notes

Work is on `codex/x1-x2-text-shapes`, based on P1–P3 commit `9f5b6c0`. Application version remains **0.4.0** and `.comp` remains **11**. Text, shape and mask metadata use existing fields; font substitutions are user preferences rather than new project fields.

| Task | Delivered |
| --- | --- |
| X1 | Native QTextEdit input embedded in a transformed graphics proxy, with Qt IME preedit/commit/replacement support, Chinese text and UTF-16 ranges. T-click, Move double-click and the contextual Edit Text button enter canvas editing; a drag creates a paragraph. Border movement and eight resize handles work under zoom, rotation and flips; resizing reflows text, and Shift constrains the movement axis or frame ratio. Enter inserts newlines, Ctrl+Enter/Apply commits, and Escape/Cancel rolls back (Escape first clears IME preedit). Text retains local undo/redo while editing, and the entire applied edit is one document undo step. Empty new layers cancel. Tool/layer/tab changes, other edits, Save, Export and closing a project commit pending text. |
| X2 | Inline font/size/tracking/leading/alignment/color controls, with selected-range or whole-layer font/color changes. Persistent font mappings and automatic substitutions for common Mac/PostScript names resolve to installed Windows families while preserving the requested names in the document. Reset restores automatic mappings after OK; Cancel leaves settings unchanged. Basic shape resizing through layer handles, group transforms and the numeric panel redraws from existing vector metadata, preserving radius, line width, normalized line endpoints, transforms and linked/independent mask placement. Existing Shift square/circle and 15-degree line creation constraints remain supported. |

Source dimensions are checked before allocation using the existing 30,000-side and 200-million-pixel surface limits. Failed frame resizing cancels the edit; invalid shape allocations leave the original source and metadata untouched. Unedited imported text keeps its original PNG, including opening/closing the inline editor without changes. Font/color runs remain UTF-16 ranges, and missing fonts fall back to an installed family without replacing the requested project names.

## Validation

MSVC Release build and **all 11 CTest suites pass**. The new `compositor_text_shape_tests` suite contains **22 regression cases**, with **24 QtTest checks passed, 0 failed, 0 skipped** including setup/cleanup. The suite also passes with the native **Windows Qt platform**, in addition to CTest's offscreen platform. Tests explicitly load host Windows fonts because Qt's offscreen plugin does not enumerate the system font directory; no font binaries are distributed.

Coverage includes Chinese preedit and commit, Escape during composition, newline/emoji input, native text keys and local undo/redo, Apply/cancel/document undo, tool/hide/layer/save transitions, editor destruction, selected UTF-16 font/color ranges, font mapping apply/reset/cancel, paragraph reflow/Shift/movement/rotation/flips/zoom, fixed source position during text overflow, unchanged imported PNGs, save/reopen, invalid sizes, rounded corners, ellipse output, invariant line width, vector handle resizing and masks. The existing transform suite also covers multi-layer/folder transforms, and the existing canvas/UI suites cover shape creation and modal text/shape editors. The self-test launcher and delivery tooling include the new suite and the transform suite; no delivery bundle was regenerated.

A native Windows canvas screenshot was inspected for the Chinese/emoji glyphs, transparent background, translated alignment control, toolbar and paragraph handles. Local evidence is in ignored `artifacts/x1-x2-*` and `build/windows/Testing/`.

## Scope and remaining acceptance

IME tests send real Qt input-method events through the native text control; they do not automate Microsoft Pinyin's OS candidate window or prove candidate positioning for every IME, monitor scale and rotated layer. Those manual checks remain, along with Mac typography/reference comparisons and target-hardware performance for very large paragraphs/shapes.

Live editing uses a native text overlay so the caret, selection and IME preedit are readable. The overlay follows the layer transform and paragraph clipping, but is drawn above the canvas without the layer's masks, effects, blend mode or upper-layer occlusion. Apply restores the final document composition. Font mappings affect subsequent editable text rendering, not existing saved PNGs; exact Mac metrics are not promised when fonts differ. Font/color support selected runs; size, tracking, leading and alignment remain layer parameters supported by the current format.

Accepting Font Mapping reloads the native text document to resolve its fonts and resets that input's local undo stack, retaining its content and selection. Document Apply/Cancel still covers the entire edit session; font preferences persist independently of document undo.

Vector resizing here applies to the existing basic shape metadata. Free perspective/folded distortion and Image Size retain their existing rasterization behavior; arbitrary editable paths/mesh shapes are outside X1/X2. Large sources still allocate contiguous images and may be expensive during a drag. No installer, published release or portable ZIP was produced.
