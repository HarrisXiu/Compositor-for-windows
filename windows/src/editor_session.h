// SPDX-License-Identifier: MIT
#pragma once
#include "canvas_layout.h"
#include <QColor>
#include <QImage>
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

// Per-project interaction state; deliberately excluded from the .comp manifest.
struct EditorSession {
    QSet<QString> selectedLayerIDs;
    QSet<QString> collapsedLayerIDs;
    QImage selection;
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
