// SPDX-License-Identifier: MIT
#pragma once
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
    Crop
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
    double brushOpacity = 1;
    double blurRadius = 5;
    double wandTolerance = 32;
    bool wandContiguous = true;
};
} // namespace compositor
