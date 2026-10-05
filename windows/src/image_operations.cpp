// SPDX-License-Identifier: MIT
#include "image_operations.h"
#include <QBuffer>
#include <QImageWriter>
#include <QJsonArray>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <functional>

namespace compositor {
namespace {
void dimensions(QSize size) {
    require(size.width() > 0 && size.height() > 0 && size.width() <= MaxSide &&
                size.height() <= MaxSide,
            "Canvas exceeds size limits");
}
QJsonObject translated(QJsonObject transform, QPointF offset) {
    auto origin = transform.value("origin").toArray();
    transform["origin"] =
        QJsonArray{origin[0].toDouble() + offset.x(), origin[1].toDouble() + offset.y()};
    return transform;
}
QJsonObject mirrored(QJsonObject transform, double axis, bool horizontal) {
    auto origin = transform.value("origin").toArray(), size = transform.value("size").toArray();
    const int coordinate = horizontal ? 0 : 1;
    origin[coordinate] = 2 * axis - origin[coordinate].toDouble() - size[coordinate].toDouble();
    transform["origin"] = origin;
    transform["rotation"] = -transform.value("rotation").toDouble();
    const auto key = horizontal ? "flipX" : "flipY";
    transform[key] = !transform.value(key).toBool();
    return transform;
}
void guides(Document &d, const std::function<double(double, bool)> &update) {
    if (!d.metadata.contains("guides"))
        return;
    auto list = d.metadata.value("guides").toArray();
    for (int i = 0; i < list.size(); ++i) {
        auto guide = list[i].toObject();
        guide["position"] =
            update(guide.value("position").toDouble(), guide.value("axis") == "horizontal");
        list[i] = guide;
    }
    d.metadata["guides"] = list;
}
QRect scaledBounds(const Layer &layer, const QSize &source, const QTransform &scale) {
    const auto box = (layer.placement(source) * scale).mapRect(QRectF(QPointF(), source));
    return QRect(QPoint(int(std::floor(box.left())), int(std::floor(box.top()))),
                 QPoint(int(std::ceil(box.right())) - 1, int(std::ceil(box.bottom())) - 1));
}
bool preserveUniformMask(const Layer &layer) {
    if (layer.mask.size() != QSize(1, 1))
        return false;
    const auto placement = layer.metadata.value("maskPlacement").isObject()
                               ? layer.metadata.value("maskPlacement").toObject()
                               : layer.transform();
    // A placed/folder mask still has a rotated coverage rectangle, even with one pixel.
    return placement.value("rotation").toDouble() == 0 ||
           (!layer.metadata.value("maskPlacement").isObject() && !layer.image.isNull());
}
QImage rasterize(const QImage &source, const Layer &layer, const QTransform &scale, QRect bounds,
                 Qt::TransformationMode sampling) {
    dimensions(bounds.size());
    require(qint64(bounds.width()) * bounds.height() <= MaxSurfacePixels,
            "Resized asset exceeds memory limit");
    QImage result(bounds.size(), source.format() == QImage::Format_Grayscale8
                                     ? QImage::Format_Grayscale8
                                     : QImage::Format_RGBA8888_Premultiplied);
    require(!result.isNull(), "Not enough memory to resize image");
    result.fill(0);
    QTransform offset;
    offset.translate(-bounds.x(), -bounds.y());
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, sampling == Qt::SmoothTransformation);
    painter.setTransform(layer.placement(source.size()) * scale * offset);
    painter.drawImage(0, 0, source);
    return result;
}
} // namespace
void resizeCanvas(Document &d, QSize size, int anchor, std::optional<QColor> fill,
                  std::optional<QPointF> explicitOffset) {
    dimensions(size);
    require(anchor >= 0 && anchor <= 8, "Invalid canvas anchor");
    const auto old = d.size();
    const auto active = d.activeId();
    const QPointF offset = explicitOffset.value_or(
        QPointF(std::floor((size.width() - old.width()) * (anchor % 3) / 2.0),
                std::floor((size.height() - old.height()) * (anchor / 3) / 2.0)));
    for (auto &layer : d.layers) {
        layer.metadata["transform"] = translated(layer.transform(), offset);
        if (layer.metadata.value("maskPlacement").isObject())
            layer.metadata["maskPlacement"] =
                translated(layer.metadata.value("maskPlacement").toObject(), offset);
    }
    guides(d, [&](double value, bool horizontal) {
        return value + (horizontal ? offset.y() : offset.x());
    });
    d.metadata["width"] = size.width();
    d.metadata["height"] = size.height();
    d.metadata["version"] = CurrentVersion;
    if (fill && (size.width() > old.width() || size.height() > old.height())) {
        require(qint64(size.width()) * size.height() <= MaxSurfacePixels,
                "Canvas extension exceeds memory limit");
        QImage pixels(size, QImage::Format_RGBA8888_Premultiplied);
        require(!pixels.isNull(), "Not enough memory for canvas extension");
        pixels.fill(*fill);
        QPainter painter(&pixels);
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.fillRect(QRectF(offset, old), Qt::transparent);
        painter.end();
        d.addImage("Canvas Extension", pixels);
        d.layers.prepend(d.layers.takeLast());
        d.metadata["activeLayerID"] = active;
    }
    d.validateAssets();
}
void cropCanvas(Document &d, QRect bounds) {
    require(!bounds.isEmpty(), "Invalid crop rectangle");
    resizeCanvas(d, bounds.size(), 0, std::nullopt, -QPointF(bounds.topLeft()));
}
void resizeImage(Document &d, QSize size, double resolution, Qt::TransformationMode sampling) {
    dimensions(size);
    require(std::isfinite(resolution) && resolution >= 1 && resolution <= 9600,
            "Invalid resolution");
    const auto old = d.size();
    if (old == size) {
        d.metadata["resolution"] = resolution;
        return;
    }
    require(qint64(size.width()) * size.height() <= MaxSurfacePixels,
            "Resized canvas exceeds memory limit");
    QTransform scale;
    const double sx = double(size.width()) / old.width(), sy = double(size.height()) / old.height();
    scale.scale(sx, sy);
    qint64 imagePixels = 0, maskPixels = 0;
    for (const auto &layer : d.layers) {
        const auto transformedSize = layer.transform().value("size").toArray();
        const QSize source = layer.image.isNull()
                                 ? QSize(std::max(1, int(transformedSize[0].toDouble())),
                                         std::max(1, int(transformedSize[1].toDouble())))
                                 : layer.image.size();
        const auto bounds = scaledBounds(layer, source, scale);
        if (!layer.image.isNull()) {
            dimensions(bounds.size());
            imagePixels += qint64(bounds.width()) * bounds.height();
        }
        if (!layer.mask.isNull()) {
            Layer placement = layer;
            if (layer.metadata.value("maskPlacement").isObject())
                placement.metadata["transform"] = layer.metadata.value("maskPlacement");
            const auto maskBounds = layer.metadata.value("maskPlacement").isObject()
                                        ? scaledBounds(placement, layer.mask.size(), scale)
                                        : bounds;
            if (!preserveUniformMask(layer)) {
                dimensions(maskBounds.size());
                maskPixels += qint64(maskBounds.width()) * maskBounds.height();
            } else
                ++maskPixels;
        }
    }
    require(imagePixels <= documentPixelBudget() && maskPixels <= documentPixelBudget(),
            "Resized layers exceed memory limit");
    for (auto &layer : d.layers) {
        const auto original = layer;
        auto transformSize = original.transform().value("size").toArray();
        const auto sourceSize = original.image.isNull()
                                    ? QSize(std::max(1, int(transformSize[0].toDouble())),
                                            std::max(1, int(transformSize[1].toDouble())))
                                    : original.image.size();
        const auto bounds = scaledBounds(original, sourceSize, scale);
        require(!bounds.isEmpty(), "Invalid resized layer bounds");
        if (!original.image.isNull()) {
            layer.image = rasterize(original.image, original, scale, bounds, sampling);
            // The original image-size operation rasterizes independently transformed layers.
            layer.metadata.remove("text");
            layer.metadata.remove("shape");
        }
        layer.metadata["transform"] = makeTransform(bounds);
        auto transform = layer.transform();
        transform["sampling"] = sampling == Qt::FastTransformation ? "Nearest" : "High quality";
        layer.metadata["transform"] = transform;
        if (!original.mask.isNull()) {
            Layer placement = original;
            const bool independent = original.metadata.value("maskPlacement").isObject();
            if (independent)
                placement.metadata["transform"] = original.metadata.value("maskPlacement");
            const auto maskBounds =
                independent ? scaledBounds(placement, original.mask.size(), scale) : bounds;
            if (!preserveUniformMask(original))
                layer.mask = rasterize(original.mask, placement, scale, maskBounds, sampling);
            if (independent)
                layer.metadata["maskPlacement"] = makeTransform(maskBounds);
        }
    }
    guides(d, [&](double value, bool horizontal) { return value * (horizontal ? sy : sx); });
    d.metadata["width"] = size.width();
    d.metadata["height"] = size.height();
    d.metadata["resolution"] = resolution;
    d.metadata["version"] = CurrentVersion;
    d.validateAssets();
}
QRect trimBounds(const QImage &image, const TrimOptions &options) {
    if (image.isNull() || !(options.top || options.bottom || options.left || options.right))
        return {};
    const auto sample =
        image.pixelColor(options.basis == TrimOptions::Basis::BottomRight ? image.width() - 1 : 0,
                         options.basis == TrimOptions::Basis::BottomRight ? image.height() - 1 : 0);
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto color = image.pixelColor(x, y);
            const bool matches =
                options.basis == TrimOptions::Basis::Transparent
                    ? color.alpha() == 0
                    : std::abs(color.red() - sample.red()) <= options.tolerance &&
                          std::abs(color.green() - sample.green()) <= options.tolerance &&
                          std::abs(color.blue() - sample.blue()) <= options.tolerance &&
                          std::abs(color.alpha() - sample.alpha()) <= options.tolerance;
            if (!matches) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    if (right < 0)
        return {};
    return QRect(QPoint(options.left ? left : 0, options.top ? top : 0),
                 QPoint(options.right ? right : image.width() - 1,
                        options.bottom ? bottom : image.height() - 1));
}
void flipCanvas(Document &d, bool horizontal) {
    const double axis = (horizontal ? d.size().width() : d.size().height()) / 2.0;
    for (auto &layer : d.layers) {
        layer.metadata["transform"] = mirrored(layer.transform(), axis, horizontal);
        if (layer.metadata.value("maskPlacement").isObject())
            layer.metadata["maskPlacement"] =
                mirrored(layer.metadata.value("maskPlacement").toObject(), axis, horizontal);
    }
    guides(d, [&](double value, bool guideHorizontal) {
        return guideHorizontal != horizontal ? 2 * axis - value : value;
    });
    d.metadata["version"] = CurrentVersion;
}
QByteArray encodeJpeg(const QImage &image, int quality, const QColor &matte) {
    require(!image.isNull(), "Nothing to export");
    QImage flat(image.size(), QImage::Format_RGB32);
    require(!flat.isNull(), "Not enough memory to export");
    flat.fill(matte.isValid() ? matte.rgb() : QColor(Qt::white).rgb());
    QPainter painter(&flat);
    painter.drawImage(0, 0, image);
    painter.end();
    flat.setDotsPerMeterX(image.dotsPerMeterX());
    flat.setDotsPerMeterY(image.dotsPerMeterY());
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "JPEG");
    writer.setQuality(std::clamp(quality, 1, 100));
    require(writer.write(flat), "Cannot encode exported image");
    return bytes;
}
} // namespace compositor
