# P1–P3 delivery notes

Application version remains **0.4.0** and `.comp` remains **11**. The implementation uses existing image assets, transforms and mask placements; it introduces no saved fields. Work is on `codex/p1-p3-painting`.

| Task | Delivered |
| --- | --- |
| P1 | Retained and verified brush/eraser smoothing and Shift-click straight lines. All dab-based tools now interpolate to the release position, including when no move event arrived there. Fine brushes use closer spacing, and long segments no longer stop at 1,000 samples. A stroke retains its opacity cap across surface growth. |
| P2 | Brush, eraser, clone and spot-heal strokes expand their source grid as needed within the canvas and selection bounds. Padding preserves old pixel positions under rotation, nonuniform scale and flips. Image growth expands an implicit mask with white coverage without stretching its existing coverage; an explicitly placed mask stays in place. Painting a mask can grow it independently, including folder masks. Small implicit/solid masks acquire an editable pixel grid. Growth and painting form one undo step; Escape and failures restore the original assets and metadata. Strokes that change no pixels produce no history entry or unsaved marker. |
| P3 | Verified aligned/un-aligned and active/all-visible-layer clone sampling, linear/radial gradients, reversed foreground/background or transparent endpoints, opacity and foreground/background fills. Clone samples retain their original placement throughout a growing stroke; transparent samples use source-over blending, negative source coordinates are rejected, and cancellation restores clone alignment. Radial gradients now use document coordinates, keeping a circle on nonuniformly scaled layers. Gradients and color fills grow to cover the canvas or selection and support independently placed masks. Zero-length gradients cancel. |

The shared `paint_surface.cpp/.h` helpers check source dimensions and the existing 200-million-pixel surface limit before padding. Bounds conversion rejects noninvertible and excessive transforms before integer conversion. Stroke selection bounds are computed once per gesture. Expanded surfaces rebuild their affected render caches; ordinary dabs retain incremental updates.

## Validation

MSVC Release build and all **10 CTest suites pass**. The canvas suite has **33 passed, 0 failed**, including **13 new regression cases**. Existing coverage verifies smoothing, Shift lines, clone alignment and merged sampling. New coverage checks four-edge growth, transformed pixel/mask placement, repeated-stroke opacity, fractional selection coverage, release endpoints, folder/independent/one-pixel masks, clone source-over and growth, radial/linear gradients, color fills, no-op edits, memory limits, undo/redo, Escape, and save/reopen. An offscreen Qt screenshot was inspected for the expanded brush output. Local evidence is under ignored `artifacts/p1-p3-*` and `build/windows/Testing/`.

## Scope and remaining acceptance

Brush dynamics and pressure, history memory budgeting and target-hardware large-document performance remain separate tasks. Blur, Smudge and Liquify retain their current fixed source bounds. Windows masks retain black coverage outside their assets; matching the Mac app's inferred edge background is still part of visual parity acceptance. Padding reallocates contiguous images and stroke buffers, so this work does not claim sparse-tile painting or constant-memory growth. No Mac comparison, installer, published release or portable ZIP was produced.
