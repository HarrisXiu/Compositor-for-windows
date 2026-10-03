// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QColor>
#include <optional>
namespace compositor {
struct TrimOptions {
    enum class Basis { Transparent, TopLeft, BottomRight };
    Basis basis = Basis::Transparent;
    bool top = true, bottom = true, left = true, right = true;
    int tolerance = 0;
};
void resizeCanvas(Document &document, QSize size, int anchor = 4,
                  std::optional<QColor> fill = std::nullopt,
                  std::optional<QPointF> offset = std::nullopt);
void cropCanvas(Document &document, QRect bounds);
void resizeImage(Document &document, QSize size, double resolution,
                 Qt::TransformationMode sampling = Qt::SmoothTransformation);
QRect trimBounds(const QImage &image, const TrimOptions &options = {});
void flipCanvas(Document &document, bool horizontal);
} // namespace compositor
