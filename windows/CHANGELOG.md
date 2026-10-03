# Windows preview changes

## Unreleased

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
