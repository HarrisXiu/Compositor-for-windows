// SPDX-License-Identifier: MIT
#include "selection_float.h"
#include "paint_surface.h"
#include "render.h"
#include <QJsonArray>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {
namespace {
QPointF pairOf(const QJsonObject &o, const char *key) {
    const auto a = o.value(QLatin1String(key)).toArray();
    return {a.at(0).toDouble(), a.at(1).toDouble()};
}
QRect coveredBounds(const QImage &coverage) {
    int left = coverage.width(), top = coverage.height(), right = -1, bottom = -1;
    for (int y = 0; y < coverage.height(); ++y) {
        const auto row = coverage.constScanLine(y);
        for (int x = 0; x < coverage.width(); ++x)
            if (row[x]) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = y;
            }
    }
    return right < 0 ? QRect() : QRect(left, top, right - left + 1, bottom - top + 1);
}
// A mask as a coverage image to draw: every channel the mask value.
QImage coverageImage(const QImage &mask) {
    QImage out(mask.size(), QImage::Format_RGBA8888_Premultiplied);
    require(!out.isNull(), "Not enough memory for selection");
    for (int y = 0; y < out.height(); ++y) {
        auto p = out.scanLine(y);
        const auto m = mask.constScanLine(y);
        for (int x = 0; x < out.width(); ++x)
            std::memset(p + x * 4, m[x], 4);
    }
    return out;
}
bool wholePixelShift(const QTransform &map, QPoint *shift) {
    if (map.type() > QTransform::TxTranslate)
        return false;
    if (std::abs(map.dx() - std::round(map.dx())) > 1e-6 ||
        std::abs(map.dy() - std::round(map.dy())) > 1e-6)
        return false;
    *shift = QPoint(int(std::lround(map.dx())), int(std::lround(map.dy())));
    return true;
}
// `image` with the mask multiplied in, so it can be drawn without one.
QImage bakedFloating(const Layer &floating) {
    auto image = floating.image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (floating.mask.isNull())
        return image;
    auto mask = floating.mask.convertToFormat(QImage::Format_Grayscale8);
    if (mask.size() != image.size())
        mask = mask.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    image.detach();
    for (int y = 0; y < image.height(); ++y) {
        auto p = image.scanLine(y);
        const auto m = mask.constScanLine(y);
        for (int x = 0; x < image.width(); ++x)
            for (int c = 0; c < 4; ++c)
                p[x * 4 + c] = uchar((int(p[x * 4 + c]) * m[x] + 127) / 255);
    }
    return image;
}
} // namespace

QImage shiftSelection(const QImage &selection, QPoint offset) {
    if (selection.isNull())
        return {};
    QImage out(selection.size(), QImage::Format_Grayscale8);
    require(!out.isNull(), "Not enough memory for selection");
    out.fill(0);
    const int width = selection.width(), height = selection.height();
    const int from = std::max(0, -offset.x()), to = std::min(width, width - offset.x());
    if (to <= from)
        return out;
    for (int y = 0; y < height; ++y) {
        const int sy = y - offset.y();
        if (sy < 0 || sy >= height)
            continue;
        std::memcpy(out.scanLine(y) + from + offset.x(), selection.constScanLine(sy) + from,
                    size_t(to - from));
    }
    return out;
}

SelectionOutline wholeSelection(const EditorSession &session) {
    if (session.selection.isNull())
        return {};
    if (auto it = session.outlines.constFind(session.selection.cacheKey());
        it != session.outlines.cend())
        return *it;
    return {session.selection, {}};
}
QImage movedSelection(EditorSession &session, const SelectionOutline &whole, QPoint offset,
                      QSize canvas, bool remember) {
    if (whole.image.isNull())
        return {};
    const QPoint origin = whole.origin + offset;
    QImage part(canvas, QImage::Format_Grayscale8);
    require(!part.isNull(), "Not enough memory for selection");
    part.fill(0);
    const auto shown = QRect(origin, whole.image.size()) & QRect(QPoint(), canvas);
    for (int y = shown.top(); y <= shown.bottom(); ++y)
        std::memcpy(part.scanLine(y) + shown.left(),
                    whole.image.constScanLine(y - origin.y()) + (shown.left() - origin.x()),
                    size_t(shown.width()));
    if (!remember)
        return part;
    const auto covered = coveredBounds(whole.image);
    if (covered.isEmpty() || QRect(QPoint(), canvas).contains(covered.translated(origin)))
        return part;
    // Only what it covers is kept, so a selection mostly past the canvas costs no more than that.
    const auto key = part.cacheKey();
    session.outlines.insert(key, {whole.image.copy(covered), origin + covered.topLeft()});
    session.outlineOrder.removeAll(key);
    session.outlineOrder.append(key);
    qint64 total = 0;
    for (const auto &kept : std::as_const(session.outlines))
        total += kept.image.sizeInBytes();
    constexpr qint64 Budget = 256LL * 1024 * 1024;
    while (session.outlineOrder.size() > 1 && total > Budget) {
        const auto oldest = session.outlineOrder.takeFirst();
        total -= session.outlines.value(oldest).image.sizeInBytes();
        session.outlines.remove(oldest);
    }
    return part;
}

QString liftSelection(Document &document, const QString &sourceId, const QImage &selection,
                      bool duplicate) {
    require(selection.size() == document.size() && selection.format() == QImage::Format_Grayscale8,
            "Selection must match the canvas size");
    auto *source = document.find(sourceId);
    require(source && !source->group() && !source->image.isNull(), "Select a pixel layer");
    const auto coverage = layerSelection(document, *source, selection);
    const auto region = coveredBounds(coverage);
    if (region.isEmpty())
        return {};
    auto base = source->image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const auto payload = base.copy(region);
    const auto mask = coverage.copy(region);
    if (!duplicate) {
        base.detach();
        for (int y = region.top(); y <= region.bottom(); ++y) {
            auto p = base.scanLine(y);
            const auto m = coverage.constScanLine(y);
            for (int x = region.left(); x <= region.right(); ++x)
                for (int c = 0; c < 4; ++c)
                    p[x * 4 + c] = uchar((int(p[x * 4 + c]) * (255 - m[x]) + 127) / 255);
        }
    }
    // The floating layer's box is the region's, scaled and turned as the source is.
    const auto placement = source->placement(source->image.size());
    auto transform = source->transform();
    const auto scale = pairOf(transform, "size");
    const QSizeF size(region.width() * scale.x() / source->image.width(),
                      region.height() * scale.y() / source->image.height());
    const auto center =
        placement.map(QPointF(region.x() + region.width() / 2.0, region.y() + region.height() / 2.0));
    transform["origin"] = QJsonArray{center.x() - size.width() / 2, center.y() - size.height() / 2};
    transform["size"] = QJsonArray{size.width(), size.height()};
    const auto id = newId();
    Layer floating;
    floating.metadata = {{"id", id},
                         {"name", "Floating Selection"},
                         {"isVisible", true},
                         {"imageFile", id + ".png"},
                         {"maskFile", id + ".mask.png"},
                         {"transform", transform},
                         {"opacity", source->opacity()},
                         {"blendMode", source->blend()}};
    if (!source->parent().isEmpty())
        floating.metadata["parentID"] = source->parent();
    floating.image = payload;
    floating.mask = mask;
    if (!duplicate) {
        source->image = base;
        source->metadata.remove("text");
        source->metadata.remove("shape");
    }
    const auto index = int(source - document.layers.data());
    document.layers.insert(index + 1, floating);
    document.metadata["version"] = CurrentVersion;
    return id;
}

void mergeFloatingLayer(Document &document, const QString &floatingId, const QString &sourceId) {
    const auto *floating = document.find(floatingId);
    auto *source = document.find(sourceId);
    require(floating && source && !source->image.isNull() && floatingId != sourceId,
            "Floating selection is no longer available");
    const auto baked = bakedFloating(*floating);
    const auto placed = floating->placement(floating->image.size());
    const auto box = placed.mapRect(QRectF(QPointF(), floating->image.size()));
    const auto sampling = floating->transform().value("sampling").toString("High quality");
    growPaintSurface(*source, false, paintSurfaceBounds(*source, box));
    // The floating layer is looked up again: nothing above has moved it, but the grow touched `source`.
    floating = document.find(floatingId);
    const auto toSource = placed * source->placement(source->image.size()).inverted();
    QPoint shift;
    QPainter painter(&source->image);
    if (wholePixelShift(toSource, &shift))
        painter.drawImage(shift, baked);
    else {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, sampling != "Nearest");
        painter.setTransform(toSource);
        painter.drawImage(0, 0, baked);
    }
    painter.end();
    source->metadata.remove("text");
    source->metadata.remove("shape");
    document.layers.erase(document.layers.begin() + (floating - document.layers.data()));
    document.metadata["activeLayerID"] = sourceId;
}

QImage floatingSelection(const Document &document, const Layer &floating) {
    QImage out(document.size(), QImage::Format_Grayscale8);
    require(!out.isNull(), "Not enough memory for selection");
    out.fill(0);
    if (floating.mask.isNull())
        return out;
    Layer maskLayer = floating;
    if (floating.metadata.value("maskPlacement").isObject())
        maskLayer.metadata["transform"] = floating.metadata.value("maskPlacement");
    const auto placed = maskLayer.placement(floating.mask.size());
    QImage canvas(document.size(), QImage::Format_RGBA8888_Premultiplied);
    require(!canvas.isNull(), "Not enough memory for selection");
    canvas.fill(Qt::transparent);
    const auto coverage = coverageImage(floating.mask.convertToFormat(QImage::Format_Grayscale8));
    QPoint shift;
    QPainter painter(&canvas);
    if (wholePixelShift(placed, &shift))
        painter.drawImage(shift, coverage);
    else {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setTransform(placed);
        painter.drawImage(0, 0, coverage);
    }
    painter.end();
    for (int y = 0; y < out.height(); ++y) {
        const auto p = canvas.constScanLine(y);
        auto o = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x)
            o[x] = p[x * 4 + 3];
    }
    return out;
}
} // namespace compositor
