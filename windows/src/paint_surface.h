// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QRect>

namespace compositor {
Layer paintTarget(const Layer &layer, bool mask);
QRect paintSurfaceBounds(const Layer &target, QRectF documentArea);
// Pads the source grid without resampling its pixels or changing their document positions.
// Returns the old pixels' offset in the new grid (zero when no padding is needed).
QPoint growPaintSurface(Layer &layer, bool mask, QRect bounds);
void preparePaintMask(Layer &layer);
} // namespace compositor
