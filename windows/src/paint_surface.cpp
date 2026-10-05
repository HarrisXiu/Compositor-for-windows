// SPDX-License-Identifier: MIT
#include "paint_surface.h"
#include "render.h"
#include <QJsonArray>
#include <QPainter>
#include <cmath>

namespace compositor {
namespace {
void checkSize(QSize size) {
    require(size.width() > 0 && size.height() > 0 && size.width() <= MaxSide &&
                size.height() <= MaxSide && qint64(size.width()) * size.height() <= MaxSurfacePixels,
            "Paint surface exceeds size or memory limits");
}
QImage padded(const QImage &image, QRect bounds, int background) {
    checkSize(bounds.size());
    QImage result(bounds.size(), image.format());
    require(!result.isNull(), "Not enough memory to expand paint surface");
    result.fill(background);
    QPainter p(&result);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(-bounds.topLeft(), image);
    return result;
}
} // namespace
Layer paintTarget(const Layer &layer, bool mask) {
    Layer target = layer;
    if (mask) {
        target.image = layer.mask;
        if (layer.metadata.value("maskPlacement").isObject())
            target.metadata["transform"] = layer.metadata["maskPlacement"];
    }
    return target;
}
QRect paintSurfaceBounds(const Layer &target, QRectF documentArea) {
    bool invertible = false;
    const auto inverse = target.placement(target.image.size()).inverted(&invertible);
    require(invertible, "Paint surface transform is not invertible");
    const auto local = inverse.mapRect(documentArea);
    require(std::isfinite(local.x()) && std::isfinite(local.y()) &&
                std::isfinite(local.width()) && std::isfinite(local.height()) &&
                std::abs(local.x()) < 100000000 && std::abs(local.y()) < 100000000 &&
                local.width() <= MaxSide && local.height() <= MaxSide,
            "Paint surface exceeds size or memory limits");
    const auto bounds = local.toAlignedRect() | target.image.rect();
    checkSize(bounds.size());
    return bounds;
}
void preparePaintMask(Layer &layer) {
    if (layer.mask.isNull())
        return;
    QSize size = layer.mask.size();
    if (!layer.metadata.value("maskPlacement").isObject() && !layer.image.isNull())
        size = layer.image.size();
    else if (size.width() <= 2 && size.height() <= 2) {
        const auto extent = paintTarget(layer, true).transform()["size"].toArray();
        size = {std::max(1, qRound(extent[0].toDouble())),
                std::max(1, qRound(extent[1].toDouble()))};
    }
    checkSize(size);
    if (size != layer.mask.size()) {
        auto image = layer.mask.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        require(!image.isNull(), "Not enough memory for paint mask");
        layer.mask = image;
    }
}
QPoint growPaintSurface(Layer &layer, bool mask, QRect bounds) {
    const auto target = paintTarget(layer, mask);
    const auto source = target.image;
    require(!source.isNull(), "Select a pixel layer or mask");
    bounds |= source.rect();
    if (bounds == source.rect())
        return {};
    // A grown mask keeps its edge tone in the new area, as on the Mac.
    auto image = padded(source, bounds, mask ? maskBackground(source) : 0);
    auto transform = target.transform();
    const auto oldSize = transform["size"].toArray();
    const QSizeF size(bounds.width() * oldSize[0].toDouble() / source.width(),
                      bounds.height() * oldSize[1].toDouble() / source.height());
    const auto center = target.placement(source.size()).map(
        QPointF(bounds.x() + bounds.width() / 2.0, bounds.y() + bounds.height() / 2.0));
    transform["origin"] = QJsonArray{center.x() - size.width() / 2,
                                    center.y() - size.height() / 2};
    transform["size"] = QJsonArray{size.width(), size.height()};
    if (mask) {
        layer.mask = image;
        layer.metadata["maskPlacement"] = transform;
    } else {
        if (!layer.mask.isNull() && !layer.metadata.value("maskPlacement").isObject()) {
            // An implicit mask shares the image grid. New pixels have no old mask restriction.
            preparePaintMask(layer);
            layer.mask = padded(layer.mask, bounds, 255);
        }
        layer.image = image;
        layer.metadata["transform"] = transform;
    }
    return -bounds.topLeft();
}
} // namespace compositor
