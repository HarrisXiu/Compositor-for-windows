// SPDX-License-Identifier: MIT
#include "distort.h"
#include "editable_layers.h"
#include "render.h"
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace compositor {
namespace {
double number(const QJsonObject &o, const char *key, double fallback = 0) {
    return o.value(QLatin1String(key)).toDouble(fallback);
}
QPointF pair(const QJsonObject &o, const char *key) {
    auto a = o.value(QLatin1String(key)).toArray();
    return a.size() == 2 ? QPointF(a[0].toDouble(), a[1].toDouble()) : QPointF();
}
double area(QPointF a, QPointF b, QPointF c) {
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
}
// The box (unflipped) as a map from the unit square to the document.
QTransform unitToBox(const QJsonObject &t) {
    const auto o = pair(t, "origin"), s = pair(t, "size");
    QTransform map;
    map.translate(o.x() + s.x() / 2, o.y() + s.y() / 2);
    map.rotate(number(t, "rotation"));
    map.scale(s.x(), s.y());
    map.translate(-.5, -.5);
    return map;
}
// Where each corner of the image's own pixels lands: a flipped layer shows its pixels mirrored, so
// they go to the opposite corners of the shape.
QVector<QPointF> imageCorners(const QVector<QPointF> &corners, bool flipX, bool flipY) {
    static constexpr int units[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    static constexpr int order[4] = {0, 1, 3, 2};
    QVector<QPointF> result;
    for (const auto &unit : units) {
        const int u = flipX ? 1 - unit[0] : unit[0], v = flipY ? 1 - unit[1] : unit[1];
        result << corners[order[v * 2 + u]];
    }
    return result;
}
// The affine map taking three source points to three destination points.
bool affine(const QPointF (&from)[3], const QPointF (&to)[3], QTransform &result) {
    const auto u = from[1] - from[0], v = from[2] - from[0];
    const auto uu = to[1] - to[0], vv = to[2] - to[0];
    const double det = u.x() * v.y() - v.x() * u.y();
    if (std::abs(det) < 1e-9)
        return false;
    const double a = (uu.x() * v.y() - vv.x() * u.y()) / det,
                 c = (vv.x() * u.x() - uu.x() * v.x()) / det,
                 b = (uu.y() * v.y() - vv.y() * u.y()) / det,
                 d = (vv.y() * u.x() - uu.y() * v.x()) / det;
    result = QTransform(a, b, c, d, to[0].x() - (a * from[0].x() + c * from[0].y()),
                        to[0].y() - (b * from[0].x() + d * from[0].y()));
    return true;
}
QJsonObject uprightTransform(const QRectF &rect, const QString &sampling) {
    return {{"origin", QJsonArray{rect.x(), rect.y()}},
            {"size", QJsonArray{rect.width(), rect.height()}},
            {"rotation", 0},
            {"flipX", false},
            {"flipY", false},
            {"sampling", sampling}};
}
} // namespace

QVector<QPointF> transformCorners(const QJsonObject &transform) {
    const auto map = unitToBox(transform);
    return {map.map(QPointF(0, 0)), map.map(QPointF(1, 0)), map.map(QPointF(1, 1)),
            map.map(QPointF(0, 1))};
}
bool distortUsable(const QVector<QPointF> &c) {
    if (c.size() != 4)
        return false;
    for (const auto &p : c)
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || std::abs(p.x()) > 1000000 ||
            std::abs(p.y()) > 1000000)
            return false;
    // Both halves need area, or one of them has nothing to draw.
    return std::abs(area(c[0], c[1], c[2])) > .01 && std::abs(area(c[0], c[2], c[3])) > .01;
}
bool distortConvex(const QVector<QPointF> &c) {
    if (!distortUsable(c))
        return false;
    double sign = 0;
    for (int i = 0; i < 4; ++i) {
        const auto a = c[i], b = c[(i + 1) % 4], d = c[(i + 2) % 4];
        const double cross = (b.x() - a.x()) * (d.y() - b.y()) - (b.y() - a.y()) * (d.x() - b.x());
        if (std::abs(cross) <= .01)
            return false;
        if (sign == 0)
            sign = cross < 0 ? -1 : 1;
        else if ((cross < 0) != (sign < 0))
            return false;
    }
    return true;
}
QPointF distortMap(const QVector<QPointF> &c, QPointF unit) {
    const double sx = c[0].x() - c[1].x() + c[2].x() - c[3].x(),
                 sy = c[0].y() - c[1].y() + c[2].y() - c[3].y();
    double g = 0, h = 0;
    if (std::abs(sx) > 1e-9 || std::abs(sy) > 1e-9) {
        const double dx1 = c[1].x() - c[2].x(), dx2 = c[3].x() - c[2].x(),
                     dy1 = c[1].y() - c[2].y(), dy2 = c[3].y() - c[2].y();
        const double den = dx1 * dy2 - dx2 * dy1;
        if (std::abs(den) > 1e-12) {
            g = (sx * dy2 - dx2 * sy) / den;
            h = (dx1 * sy - sx * dy1) / den;
        }
    }
    const double a = c[1].x() - c[0].x() + g * c[1].x(), b = c[3].x() - c[0].x() + h * c[3].x(),
                 d = c[1].y() - c[0].y() + g * c[1].y(), e = c[3].y() - c[0].y() + h * c[3].y();
    const double w = g * unit.x() + h * unit.y() + 1;
    return {(a * unit.x() + b * unit.y() + c[0].x()) / w,
            (d * unit.x() + e * unit.y() + c[0].y()) / w};
}
QVector<QPointF> carriedCorners(const QJsonObject &placement, const QJsonObject &by,
                                const QVector<QPointF> &corners) {
    const auto toUnit = unitToBox(by).inverted();
    QVector<QPointF> result;
    for (const auto &p : transformCorners(placement))
        result << distortMap(corners, toUnit.map(p));
    return result;
}

QTransform unitPlacement(const QJsonObject &transform) {
    Layer layer;
    layer.metadata["transform"] = transform;
    return layer.placement(QSize(1, 1));
}
QJsonObject placedLike(const QJsonObject &like, const QTransform &map) {
    const double sign = like.value("flipX").toBool() ? -1 : 1;
    const double angle = std::atan2(map.m12() * sign, map.m11() * sign);
    const double along = -map.m21() * std::sin(angle) + map.m22() * std::cos(angle);
    const auto middle = map.map(QPointF(.5, .5));
    const QPointF size(std::hypot(map.m11(), map.m12()), std::abs(along));
    const double degrees = angle * 180 / std::numbers::pi;
    auto result = like;
    result["size"] = QJsonArray{size.x(), size.y()};
    result["rotation"] = degrees + std::round((number(like, "rotation") - degrees) / 360) * 360;
    result["flipY"] = along < 0;
    result["origin"] = QJsonArray{middle.x() - size.x() / 2, middle.y() - size.y() / 2};
    return result;
}
QJsonObject followedTransform(const QJsonObject &placement, const QJsonObject &from,
                              const QJsonObject &to) {
    if (from == to)
        return placement;
    // A plain move carries exactly.
    if (pair(from, "size") == pair(to, "size") && number(from, "rotation") == number(to, "rotation") &&
        from.value("flipX").toBool() == to.value("flipX").toBool() &&
        from.value("flipY").toBool() == to.value("flipY").toBool()) {
        auto result = placement;
        const auto origin = pair(placement, "origin") + pair(to, "origin") - pair(from, "origin");
        result["origin"] = QJsonArray{origin.x(), origin.y()};
        return result;
    }
    return placedLike(placement, unitPlacement(placement) * unitPlacement(from).inverted() *
                                     unitPlacement(to));
}
QJsonObject uprightBox(const QVector<QJsonObject> &transforms) {
    QRectF box;
    bool any = false;
    for (const auto &t : transforms)
        for (const auto &p : transformCorners(t)) {
            if (!any)
                box = QRectF(p, QSizeF(0, 0));
            any = true;
            box.setLeft(std::min(box.left(), p.x()));
            box.setTop(std::min(box.top(), p.y()));
            box.setRight(std::max(box.right(), p.x()));
            box.setBottom(std::max(box.bottom(), p.y()));
        }
    if (!any)
        return {};
    box.setSize({std::max(1.0, box.width()), std::max(1.0, box.height())});
    return uprightTransform(box, "High quality");
}
void retransformLayer(Layer &layer, const QJsonObject &moved) {
    const auto old = layer.transform();
    const auto placement = layer.metadata.value("maskPlacement");
    QImage resized;
    if (layer.metadata.value("shape").isObject() && old.value("size") != moved.value("size")) {
        const auto size = moved.value("size").toArray();
        const double width = std::ceil(size[0].toDouble()), height = std::ceil(size[1].toDouble());
        require(std::isfinite(width) && std::isfinite(height) && width >= 1 && height >= 1 &&
                    width <= MaxSide && height <= MaxSide && width * height <= MaxSurfacePixels,
                "Shape exceeds size limit");
        resized = renderShape(layer.metadata.value("shape").toObject(), {int(width), int(height)});
    }
    layer.metadata["transform"] = moved;
    if (layer.metadata.value("maskLinked").toBool(true) && placement.isObject())
        layer.metadata["maskPlacement"] = followedTransform(placement.toObject(), old, moved);
    if (!resized.isNull()) {
        if (!layer.mask.isNull() && !placement.isObject())
            layer.metadata["maskPlacement"] = moved;
        layer.image = resized;
    }
}

Warped warpImage(const QImage &image, const QJsonObject &transform,
                 const QVector<QPointF> &corners, bool mask, double limit) {
    require(distortUsable(corners), "Distortion collapses the layer");
    double minX = corners[0].x(), maxX = minX, minY = corners[0].y(), maxY = minY;
    for (const auto &p : corners) {
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }
    minX = std::floor(minX);
    minY = std::floor(minY);
    const double width = std::ceil(maxX) - minX, height = std::ceil(maxY) - minY;
    require(width >= 1 && height >= 1 && width <= 300000 && height <= 300000 &&
                width * height <= double(MaxSurfacePixels),
            "Distorted layer exceeds 200 megapixel limit");
    const auto sampling = transform.value("sampling").toString("High quality");
    const auto placed = uprightTransform(QRectF(minX, minY, width, height), sampling);
    // A uniform 1 × 1 mask already covers any shape.
    if (mask && image.size() == QSize(1, 1))
        return {image, placed, QRect(0, 0, 1, 1)};
    const double factor = limit > 0 ? std::min(1.0, limit / std::max(width, height)) : 1;
    const int outWidth = std::max(1, int(std::ceil(width * factor))),
              outHeight = std::max(1, int(std::ceil(height * factor)));
    QImage out(outWidth, outHeight, QImage::Format_RGBA8888_Premultiplied);
    require(!out.isNull(), "Not enough memory for the distorted layer");
    // Outside the shape a mask keeps its edge tone, as on the Mac; a layer is transparent there.
    const int background = mask ? maskBackground(image) : 0;
    out.fill(mask ? QColor(background, background, background) : QColor(Qt::transparent));
    const auto source = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const auto target = imageCorners(corners, transform.value("flipX").toBool(),
                                     transform.value("flipY").toBool());
    QVector<QPointF> destination;
    for (const auto &p : target)
        destination << QPointF((p.x() - minX) * factor, (p.y() - minY) * factor);
    const QPolygonF sourceQuad{QPointF(0, 0), QPointF(source.width(), 0),
                               QPointF(source.width(), source.height()),
                               QPointF(0, source.height())};
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, sampling != "Nearest");
    if (distortConvex(corners)) {
        painter.setRenderHint(QPainter::Antialiasing);
        QTransform map;
        require(QTransform::quadToQuad(sourceQuad, QPolygonF(destination), map),
                "Distortion collapses the layer");
        painter.setTransform(map);
        painter.drawImage(0, 0, source);
    } else {
        // A folded shape has no perspective that takes the image to it, so each half goes there
        // on its own, as two triangles meeting along the shape's diagonal.
        const QPointF s[4] = {sourceQuad[0], sourceQuad[1], sourceQuad[2], sourceQuad[3]};
        const int halves[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto &half : halves) {
            const QPointF from[3] = {s[half[0]], s[half[1]], s[half[2]]},
                          to[3] = {destination[half[0]], destination[half[1]],
                                   destination[half[2]]};
            QTransform map;
            if (!affine(from, to, map))
                continue;
            painter.save();
            // Hard edges along the shared diagonal, so the halves meet instead of blending twice.
            painter.setRenderHint(QPainter::Antialiasing, false);
            QPainterPath triangle;
            triangle.addPolygon(QPolygonF{to[0], to[1], to[2], to[0]});
            painter.setClipPath(triangle);
            painter.setTransform(map);
            painter.drawImage(0, 0, source);
            painter.restore();
        }
    }
    painter.end();
    return {mask ? out.convertToFormat(QImage::Format_Grayscale8) : out, placed,
            QRect(0, 0, outWidth, outHeight)};
}
Warped warpTrimmed(const QImage &image, const QJsonObject &transform,
                   const QVector<QPointF> &corners) {
    auto warped = warpImage(image, transform, corners, false);
    const QRect full(QPoint(), warped.image.size());
    int left = full.width(), top = full.height(), right = -1, bottom = -1;
    for (int y = 0; y < full.height(); ++y) {
        const auto row = warped.image.constScanLine(y);
        for (int x = 0; x < full.width(); ++x)
            if (row[x * 4 + 3]) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = y;
            }
    }
    // Nothing visible, or nothing to trim: keep the warp as it is.
    if (right < 0 || QRect(left, top, right - left + 1, bottom - top + 1) == full)
        return warped;
    const QRect crop(left, top, right - left + 1, bottom - top + 1);
    auto origin = pair(warped.transform, "origin");
    warped.transform["origin"] = QJsonArray{origin.x() + crop.x(), origin.y() + crop.y()};
    warped.transform["size"] = QJsonArray{double(crop.width()), double(crop.height())};
    return {warped.image.copy(crop), warped.transform, crop};
}

void distortLayer(Layer &layer, const QJsonObject &transform, const QVector<QPointF> &corners,
                  double limit) {
    require(!layer.image.isNull(), "Only pixel layers can be distorted");
    const auto warped = limit > 0 ? warpImage(layer.image, transform, corners, false, limit)
                                  : warpTrimmed(layer.image, transform, corners);
    if (!layer.mask.isNull()) {
        const auto placement = layer.metadata.value("maskPlacement").toObject();
        const bool linked = layer.metadata.value("maskLinked").toBool(true);
        if (linked && placement.isEmpty()) {
            // The mask is stretched over the layer, so it takes the same shape, trimmed alike.
            if (layer.mask.size() != QSize(1, 1)) {
                auto moved = warpImage(layer.mask, transform, corners, true, limit);
                layer.mask = limit > 0 || moved.image.rect() == warped.crop
                                 ? moved.image
                                 : moved.image.copy(warped.crop);
            }
        } else if (linked) {
            // A mask placed apart takes the same perspective over its own bounds.
            const auto carried = carriedCorners(placement, transform, corners);
            if (distortUsable(carried)) {
                auto moved = warpImage(layer.mask, placement, carried, true, limit);
                layer.mask = moved.image;
                layer.metadata["maskPlacement"] = moved.transform;
            }
        } else if (placement.isEmpty()) {
            // An unlinked mask keeps its place on the document.
            layer.metadata["maskPlacement"] = transform;
        }
    }
    layer.image = warped.image;
    layer.metadata["transform"] = warped.transform;
    layer.metadata.remove("text");
    layer.metadata.remove("shape");
}
} // namespace compositor
