// SPDX-License-Identifier: MIT
#pragma once
#include "canvas_layout.h"
#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QRectF>
#include <QSet>

namespace compositor {
enum class Tool {
    Move,
    Brush,
    Erase,
    RectangleSelect,
    EllipseSelect,
    Lasso,
    Wand,
    Gradient,
    Rectangle,
    Ellipse,
    Line,
    Text,
    Eyedropper,
    Clone,
    Heal,
    Blur,
    Smudge,
    Liquify,
    Pan,
    Crop,
    Zoom,
    Select
};
enum class EditTarget { Pixels, Mask };

// A selection whole, past the canvas too: an 8-bit coverage image and its top-left on the
// document, in pixels.
struct SelectionOutline {
    QImage image;
    QPoint origin;
};

// Per-project interaction state; deliberately excluded from the .comp manifest.
struct EditorSession {
    QSet<QString> selectedLayerIDs;
    QSet<QString> collapsedLayerIDs;
    QImage selection;
    // As on the Mac, a selection moved partly off the canvas keeps what left it, so moving it back
    // restores it: the whole, by the cacheKey of the canvas-sized part `selection` (and the history)
    // holds. Any other change to the selection is a new image and leaves them behind. Bounded in
    // memory, newest kept (see movedSelection).
    QHash<qint64, SelectionOutline> outlines;
    QList<qint64> outlineOrder;
    QRectF cropFrame;
    QRectF cropBeforeGesture;
    double cropRatio = 0;
    QColor foreground = QColor(68, 157, 245);
    QColor background = Qt::white;
    EditTarget target = EditTarget::Pixels;
    Tool tool = Tool::Move;
    double brushSize = 32;
    double hardness = .8;
    double brushSmoothing = 0;
    double brushOpacity = 1;
    double blurRadius = 5;
    double wandTolerance = 32;
    bool wandContiguous = true;
    bool wandMerged = true, selectionAntialiased = true;
    bool polygonalLasso = false;
    int wandSampleSize = 1, healingMode = 0;
    CanvasViewOptions view;
    int selectionMode = 0; // Replace, add, subtract, intersect.
    bool cloneAligned = true, cloneMerged = false;
    int gradientKind = 0; // Linear, radial.
    bool gradientBackground = false, gradientReverse = false;
    double shapeCornerRadius = 0, shapeLineWidth = 2;
    QString textFont = "Segoe UI";
    double textSize = 48;
    QString textAlignment = "Left";
    double textTracking = 0, textLeading = 0;
    bool autoSelect = false;
    int pickerSize = 1;
};
} // namespace compositor
