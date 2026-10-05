// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QByteArray>
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
// A JPEG of `image` as exported: transparency filled with `matte`, at `quality` (1-100), keeping the
// image's resolution.
QByteArray encodeJpeg(const QImage &image, int quality, const QColor &matte = Qt::white);
} // namespace compositor
