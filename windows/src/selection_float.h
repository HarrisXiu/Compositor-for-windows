// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QImage>
#include <QPoint>

namespace compositor {
// Moving and transforming the pixels inside a selection (S2). The pixels are lifted onto a floating
// layer placed exactly over where they were; the selection travels with it as the floating layer's
// mask, so every layer transform (move, scale, rotate, distort) carries the selection along too.
// Merging draws the floating layer back into its source, growing the source where pixels now lie
// past its edge, and turns the mask back into a document selection.

// `selection` (canvas-sized, 8-bit) moved by whole pixels; what leaves the canvas is lost.
QImage shiftSelection(const QImage &selection, QPoint offset);
// Lifts the pixels `selection` covers on the pixel layer `sourceId` onto a new layer just above it
// and returns that layer's ID, or an empty string when the selection covers none of the layer. The
// source loses the lifted pixels (in proportion to the selection's coverage) unless `duplicate`.
QString liftSelection(Document &document, const QString &sourceId, const QImage &selection,
                      bool duplicate);
// Draws the floating layer into `sourceId` through the floating layer's own transform and removes it.
// The source grows to hold anything that now lies past its edge; its mask grows revealed.
void mergeFloatingLayer(Document &document, const QString &floatingId, const QString &sourceId);
// The selection the floating layer's mask now describes, on the canvas.
QImage floatingSelection(const Document &document, const Layer &floating);
} // namespace compositor
