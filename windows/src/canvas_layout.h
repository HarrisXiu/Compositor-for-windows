// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QColor>
#include <QSet>
namespace compositor {
struct CanvasViewOptions {
    bool rulers = false, guides = true, grid = false, snap = true, lockGuides = false;
    bool snapCanvas = true, snapLayers = true, snapGuides = true, snapGrid = false;
    bool transformControls = true;
    int gridSpacing = 64, gridSubdivisions = 8;
    QColor guideColor{55, 205, 220}, gridColor{140, 150, 160, 110};
};
CanvasViewOptions loadCanvasViewOptions();
void saveCanvasViewOptions(const CanvasViewOptions &options);
double snapCoordinate(const Document &document, const CanvasViewOptions &options, double value,
                      bool horizontal, double zoom, const QSet<QString> &exclude = {});
QPointF snapBounds(const Document &document, const CanvasViewOptions &options, const QRectF &bounds,
                   double zoom, const QSet<QString> &exclude = {});
} // namespace compositor
