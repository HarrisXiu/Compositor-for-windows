# Windows editor architecture

The 0.4 preview separates the application shell, per-project interaction state and tool dispatch. The canvas draws the document from tiles rendered for what is on screen, at full resolution when zoomed in (see Canvas rendering below).

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
| `filter_preview.cpp` / `preview_runner.cpp` | Live canvas preview of an open filter or adjustment dialog; newest-wins background jobs |
| `parameter_control.cpp` | Slider + field + scrubbable label for every numeric parameter |
| `adjustment_tools.cpp` / `adjustment_panels.cpp` / `adjustment_dialogs.cpp` | Levels, Curves, Hue/Saturation and Camera Raw panels: histograms, handles, bands, wheels, eyedroppers |
| `selection_float.cpp` / `canvas_floating.cpp` | Floating selections: lifting, transforming and merging selected pixels |
| `effect_dialogs.cpp` | Layer effect dialogs |
| `editable_dialogs.cpp` | Editable text and shape dialogs |
| `canvas.cpp` | View painting, coordinates, event routing and selection operations |
| `canvas_tools.cpp` | Concrete tools implementing press/move/release/key/overlay/cancel interface |
| `canvas_paint.cpp` | Shared painting, warp and stroke-commit kernels |
| `blend.cpp` | Parsed blend modes, Normal path and independent parallel rows |
| `render.cpp` | Compositing a document, or any part of it at any size; backdrops for repeated edits to one layer |

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

GPU rendering is not part of this change. The optional `COMPOSITOR_BENCHMARK=1 compositor_tests blendTiming` command compares the old and new blend loop on this machine.

## Canvas rendering

`renderArea(document, {full, pixels})` renders part of the document as if all of it were `full` pixels. It equals `renderDocument(document, full).copy(pixels)`, to within one level per channel where Qt resamples a transformed layer (it steps from the first pixel it draws). Blur adjustments read their neighbors, so an area is rendered with a margin of `renderReach` pixels and cropped; Add Noise and Grain are seeded by output position (`originX`/`originY`), so an area gets the same noise as the whole. Layer effects are built once per layer at its own resolution and cached (up to 512 MiB, so a large layer's effects fit).

The canvas keeps 256-pixel tiles of the document at power-of-two reductions: level n is 1/2^n of the document, rounding up. It draws from the smallest level with at least as many pixels as the screen shows (zoom times the device pixel ratio), so drawing only shrinks a level by up to half, or enlarges full size pixel for pixel. Only tiles on screen are rendered, in parallel, after `prepareRender` has built the effect and halving caches they read. Visible tiles are stitched into one image before scaling so no seams appear between them.

Ordinarily tiles render in the background, from a copy of the document (cheap: its images are shared), so opening a large document or zooming never blocks the window. A tile not rendered yet is shown meanwhile from cached tiles of a coarser level scaled up, or of the next finer one scaled down. Dropping tiles changes a generation number and cancels the batch in flight; a batch from an older generation is discarded when it ends, and the next paint asks for what is still missing. While a layer is edited, the tiles the edit changes are rendered on the UI thread instead, so each dab shows at once. `Canvas::waitForRendering` lets tests wait for a batch. At 800% and above a pixel grid is drawn.

Canvas previews pass `halvings`: a layer drawn below half size is first reduced by averaging 2×2 pixels (repeating the edge on odd sizes) as many times as needed, then resampled, so zoomed-out views stay sharp instead of aliasing. Halved images are cached by their source's `cacheKey`. `renderDocument` never halves, so exports and dialog previews are unchanged.

The canvas records each layer's metadata and image/mask cache keys when it draws. `refresh()` compares the document against that record: a changed pixel layer invalidates only the tiles over its old and new extent (`layerExtent`, which includes how far its effects reach, plus the blurs' reach); a change to a folder or adjustment, to the layer order or to the canvas size invalidates everything. A refresh after which nothing changed, such as recording a finished stroke for undo, renders nothing.

While a brush stroke or move edits one layer, each tile keeps a backdrop: everything composited before the step where that layer, the layers inside it or those clipped to them join the stack (`renderBackdrop`). Redrawing a tile then composites only that step and the ones above it over a copy (`renderOver`). Brush dabs report the document area they changed, so `refreshArea` redraws only those tiles. Any other change drops the backdrops. Dabs also report the pixels they changed in the edited image or mask, and `carryRenderCaches` moves that image's halvings, mask coverage image and layer effects over to its new version, rewriting only what the change reaches, instead of letting the next render rebuild them whole. Every effect step reaches a bounded distance (`effectsReach`: a shift plus a blur of 3 standard deviations, a dilation, or nothing), so `updateEffects` recomputes the part of the effect image within that reach of the change from the layer pixels within twice that reach, and gets exactly the pixels a full rebuild would. The tiles a dab redraws are grown by the same reach, since a shadow can fall in a tile the brush never touched.

The eyedropper renders only its full-resolution point or averaging region under the pointer. The magic wand reads a full-size composite that the canvas keeps until the document changes or another tool is chosen.

On this machine, `COMPOSITOR_BENCHMARK=1 compositor_tests tiledRenderingTiming` measures a 6000×4000 document with 20 layers: the previous 1,600-pixel preview took about 86 ms per refresh; a 1440×900 view at 100% (35 tiles) renders from nothing in about 27 ms, and a brush dab over four tiles with their backdrops in about 3 ms. At 25%, a dab's halvings of a 6000×4000 layer take about 0.1 ms to carry over, against 28 ms to rebuild. A dab on a 6000×4000 layer with a drop shadow and a stroke carries its effects over in about 31 ms, against about 15 s to rebuild them; building a large layer's effects the first time still takes that long (their blur is a direct convolution), but in the background.

## Canvas interaction and shortcuts

`canvas_interaction.cpp` owns canvas keys, guide gestures, transform handles and view-only overlays. `canvas_layout.cpp` shares snapping across move, resize, crop, shape and guide operations; preferences persist through QSettings. Guide edits reuse the existing manifest field and enter document history. Tool options and colors stay in EditorSession.

`shortcuts.cpp` registers menu actions, tools and canvas commands with stable IDs. The editor validates conflicts, persists only overrides, updates QAction bindings and maps canvas input before dispatch. Native text widgets retain typing/navigation, and the temporary Hand tool tracks its physical release key after remapping. Selection boundaries reuse a cached path; only dash phase changes on the animation timer.

## Floating selections and live previews

Selected pixels are lifted onto a temporary layer placed exactly over them, carrying the selection as its mask, so the ordinary transform handles move, scale, rotate and distort pixels and selection together. Merging draws the layer back through its transform, growing the source where needed, and turns the mask back into the selection. A floating selection is one interaction from the page's point of view: Ctrl+T emits `editStarted`, its gestures don't record history, and Enter emits a single `editFinished` while Escape's `editCanceled` restores the snapshot.

A filter dialog leaves the window usable for viewing. `FilterPreview` hands the canvas a function applied to the copy of the document it draws (`setLivePreview`), never to the document. Filters run in the background on a copy of the layer reduced to what the screen shows; the canvas redraws the previewed layer over cached backdrops, so each result shows at once. Commands from the menus cancel the dialog first; view commands don't.
