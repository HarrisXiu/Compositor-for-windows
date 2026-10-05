// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
namespace compositor {
QImage renderDocument(const Document &document, QSize outputSize = {});
QImage layerSelection(const Document &document, const Layer &layer, const QImage &selection);
QImage bakeClipping(const Document &document, const Layer &layer);

// Part of the document, rendered as if all of it were `full` pixels: renderArea(d, {full,
// pixels}) is renderDocument(d, full).copy(pixels), without rendering the rest. (Where a layer
// is resampled, Qt steps from the first pixel it draws, so a channel can round one level apart.)
struct RenderArea {
    QSize full;
    QRect pixels;
    // Shrink layer images by halving before resampling them below half size. Sharper when a
    // canvas preview is zoomed out; renderDocument never does it, so exports are unchanged.
    bool halvings = false;
    // Let compositing split rows across threads; off when areas are rendered concurrently.
    bool parallel = true;
};
QImage renderArea(const Document &document, const RenderArea &area);
// Repeated edits to one layer (a brush stroke, a move): what is composited before that layer
// joins the stack, and the rest drawn over a copy of it. renderOver(d, a, id,
// renderBackdrop(d, a, id)) is renderArea(d, a) while only that layer, or what is inside it,
// has changed in between.
QImage renderBackdrop(const Document &document, const RenderArea &area, const QString &layerId);
QImage renderOver(const Document &document, const RenderArea &area, const QString &layerId,
                  const QImage &backdrop);
// How many output pixels away, at `full` size, a pixel's value can come from: the reach of the
// blur adjustments. Areas are rendered with this much margin.
int renderReach(const Document &document, QSize full);
// Builds the per-layer caches (effects, halvings) a render at `full` reads, so concurrent
// renderArea calls don't each compute them.
void prepareRender(const Document &document, QSize full, bool halvings);
// After a layer's pixels, or with `mask` its mask, were edited in place within `changed` (pixels
// of what was edited), rebuilds only the part of the cached images a render derives from them
// (halvings, the mask's coverage image, effects) that the change reaches, updating those of the
// previous version, whose cacheKey was `previousKey`. Without them, the next render rebuilds
// them whole.
void carryRenderCaches(const Layer &layer, bool mask, qint64 previousKey, const QRect &changed);
// The document area a layer draws on, its effects included; empty when it draws no pixels.
QRectF layerExtent(const Layer &layer);
// What a mask shows beyond its pixels once placed apart from its layer (or grown, blurred or
// distorted past them): 255 or 0, whichever most of its edge is, as on the Mac, so a
// reveal-all mask keeps revealing and a hide-all mask hiding.
int maskBackground(const QImage &mask);
} // namespace compositor
