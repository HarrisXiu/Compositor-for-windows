// SPDX-License-Identifier: MIT
#pragma once
#include <QColor>
#include <QImage>

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
    QImage selection;
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
