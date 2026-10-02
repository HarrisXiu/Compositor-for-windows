// SPDX-License-Identifier: MIT
#include "render.h"
#include "effects.h"
#include "filters.h"
#include <QCache>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace compositor {
using Color = std::array<double, 3>;
static double lum(Color c) {
    return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
}
static Color setLum(Color c, double l) {
    double d = l - lum(c);
    for (auto &v : c)
        v += d;
    double lo = *std::min_element(c.begin(), c.end()), hi = *std::max_element(c.begin(), c.end());
    l = lum(c);
    if (lo < 0)
        for (auto &v : c)
            v = l + (v - l) * l / (l - lo);
    if (hi > 1)
        for (auto &v : c)
            v = l + (v - l) * (1 - l) / (hi - l);
    return c;
}
static double sat(Color c) {
    return *std::max_element(c.begin(), c.end()) - *std::min_element(c.begin(), c.end());
}
static Color setSat(Color c, double s) {
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return c[a] < c[b]; });
    int lo = order[0], mid = order[1], hi = order[2];
    if (c[hi] > c[lo]) {
        c[mid] = (c[mid] - c[lo]) * s / (c[hi] - c[lo]);
        c[hi] = s;
    } else
        c[mid] = c[hi] = 0;
    c[lo] = 0;
    return c;
}
static double channel(double b, double s, const QString &m) {
    if (m == "Multiply")
        return b * s;
    if (m == "Screen")
        return b + s - b * s;
    if (m == "Darken")
        return std::min(b, s);
    if (m == "Lighten")
        return std::max(b, s);
    if (m == "Color Burn")
        return b == 1 ? 1 : s == 0 ? 0 : 1 - std::min(1.0, (1 - b) / s);
    if (m == "Color Dodge")
        return b == 0 ? 0 : s == 1 ? 1 : std::min(1.0, b / (1 - s));
    if (m == "Linear Burn")
        return std::max(0.0, b + s - 1);
    if (m == "Linear Dodge (Add)")
        return std::min(1.0, b + s);
    if (m == "Overlay")
        return b <= 0.5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    if (m == "Hard Light")
        return s <= 0.5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    if (m == "Soft Light") {
        auto d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
        return s <= 0.5 ? b - (1 - 2 * s) * b * (1 - b) : b + (2 * s - 1) * (d - b);
    }
    if (m == "Vivid Light")
        return s <= 0.5 ? channel(b, 2 * s, "Color Burn") : channel(b, 2 * s - 1, "Color Dodge");
    if (m == "Linear Light")
        return std::clamp(b + 2 * s - 1, 0.0, 1.0);
    if (m == "Pin Light")
        return s <= 0.5 ? std::min(b, 2 * s) : std::max(b, 2 * s - 1);
    if (m == "Hard Mix")
        return channel(b, s, "Vivid Light") < 0.5 ? 0 : 1;
    if (m == "Difference")
        return std::abs(b - s);
    if (m == "Exclusion")
        return b + s - 2 * b * s;
    if (m == "Subtract")
        return std::max(0.0, b - s);
    if (m == "Divide")
        return b == 0 ? 0 : s == 0 ? 1 : std::min(1.0, b / s);
    return s;
}
static void composite(QImage &back, const QImage &front, const QString &mode, double opacity) {
    for (int y = 0; y < back.height(); ++y) {
        auto b = back.scanLine(y);
        auto s = front.constScanLine(y);
        for (int x = 0; x < back.width(); ++x, b += 4, s += 4) {
            double sa = s[3] / 255.0 * opacity, ba = b[3] / 255.0;
            if (sa <= 0)
                continue;
            Color bc{}, sc{}, blend{};
            for (int c = 0; c < 3; ++c) {
                bc[c] = ba > 0 ? b[c] / 255.0 / ba : 0;
                sc[c] = s[3] > 0 ? s[c] / double(s[3]) : 0;
            }
            if (mode == "Hue")
                blend = setLum(setSat(sc, sat(bc)), lum(bc));
            else if (mode == "Saturation")
                blend = setLum(setSat(bc, sat(sc)), lum(bc));
            else if (mode == "Color")
                blend = setLum(sc, lum(bc));
            else if (mode == "Luminosity")
                blend = setLum(bc, lum(sc));
            else
                for (int c = 0; c < 3; ++c)
                    blend[c] = channel(bc[c], sc[c], mode);
            double alpha = sa + ba - sa * ba;
            for (int c = 0; c < 3; ++c) {
                double v = (1 - sa) * b[c] / 255.0 + (1 - ba) * sc[c] * sa + sa * ba * blend[c];
                b[c] = uchar(std::clamp(std::lround(v * 255), 0L, 255L));
            }
            b[3] = uchar(std::clamp(std::lround(alpha * 255), 0L, 255L));
        }
    }
}
static QImage alphaImage(const QImage &mask) {
    QImage out(mask.size(), QImage::Format_RGBA8888_Premultiplied);
    require(!out.isNull(), "Not enough memory for mask");
    for (int y = 0; y < out.height(); ++y) {
        auto p = out.scanLine(y);
        auto m = mask.constScanLine(y);
        for (int x = 0; x < out.width(); ++x)
            for (int c = 0; c < 4; ++c)
                p[x * 4 + c] = m[x];
    }
    return out;
}
static QImage placed(const Layer &l, const QImage &image, QSize output, QSize canvas) {
    QImage surface(output, QImage::Format_RGBA8888_Premultiplied);
    require(!surface.isNull(), "Not enough memory for layer preview");
    surface.fill(Qt::transparent);
    QPainter painter(&surface);
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          l.transform().value("sampling") != "Nearest");
    QTransform scale;
    scale.scale(double(output.width()) / canvas.width(), double(output.height()) / canvas.height());
    painter.setTransform(l.placement(image.size()) * scale);
    painter.drawImage(QPointF(), image);
    return surface;
}
static void applyMask(QImage &surface, const Layer &l, QSize canvas) {
    if (l.mask.isNull() || !l.metadata.value("maskEnabled").toBool(true))
        return;
    Layer maskLayer = l;
    if (l.metadata.value("maskPlacement").isObject())
        maskLayer.metadata["transform"] = l.metadata.value("maskPlacement");
    auto coverage = placed(maskLayer, alphaImage(l.mask), surface.size(), canvas);
    QPainter painter(&surface);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(0, 0, coverage);
}
static double effectiveOpacity(const Document &d, const Layer &l) {
    double value = l.opacity();
    auto parent = l.parent();
    while (!parent.isEmpty()) {
        auto p = d.find(parent);
        if (!p)
            break;
        value *= p->opacity();
        parent = p->parent();
    }
    return value;
}
static QImage ownSurface(const Document &d, const Layer &l, QSize output) {
    auto effects = l.metadata.value("effects").toObject();
    const auto effectKeys = effects.keys();
    for (const auto &key : effectKeys)
        if (!QStringList{"stroke", "shadow", "colorOverlay", "innerShadow", "outerGlow",
                         "innerGlow"}
                 .contains(key))
            effects.remove(key);
    QImage surface;
    if (!effects.isEmpty() && !l.image.isNull()) {
        static QCache<QString, EffectImage> cache(64 * 1024);
        static QMutex mutex;
        auto cacheSettings =
            QJsonObject{{"effects", effects}, {"maskEnabled", l.metadata.value("maskEnabled")}};
        if (l.metadata.value("maskPlacement").isObject()) {
            cacheSettings["transform"] = l.transform();
            cacheSettings["maskPlacement"] = l.metadata.value("maskPlacement");
        }
        QString key =
            QString::number(l.image.cacheKey()) + ":" + QString::number(l.mask.cacheKey()) + ":" +
            QString::fromUtf8(QJsonDocument(cacheSettings).toJson(QJsonDocument::Compact));
        EffectImage built;
        {
            QMutexLocker lock(&mutex);
            if (auto hit = cache.object(key))
                built = *hit;
        }
        if (built.image.isNull()) {
            QImage shown = l.image;
            if (!l.mask.isNull() && l.metadata.value("maskEnabled").toBool(true)) {
                QImage mask(shown.size(), QImage::Format_RGBA8888_Premultiplied);
                mask.fill(Qt::transparent);
                Layer placement = l;
                if (l.metadata.value("maskPlacement").isObject())
                    placement.metadata["transform"] = l.metadata.value("maskPlacement");
                QPainter p(&mask);
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.setTransform(placement.placement(l.mask.size()) *
                               l.placement(shown.size()).inverted());
                p.drawImage(0, 0, alphaImage(l.mask));
                p.end();
                QPainter q(&shown);
                q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                q.drawImage(0, 0, mask);
            }
            built = renderEffects(shown, effects);
            qint64 cost = built.image.sizeInBytes() / 1024 + 1;
            if (cost <= 64 * 1024) {
                QMutexLocker lock(&mutex);
                cache.insert(key, new EffectImage(built), int(cost));
            }
        }
        surface = QImage(output, QImage::Format_RGBA8888_Premultiplied);
        require(!surface.isNull(), "Not enough memory for effects");
        surface.fill(Qt::transparent);
        QPainter p(&surface);
        p.setRenderHint(QPainter::SmoothPixmapTransform,
                        l.transform().value("sampling") != "Nearest");
        QTransform translation, scale;
        translation.translate(-built.inset, -built.inset);
        scale.scale(double(output.width()) / d.size().width(),
                    double(output.height()) / d.size().height());
        p.setTransform(translation * l.placement(l.image.size()) * scale);
        p.drawImage(0, 0, built.image);
    } else {
        surface = placed(l, l.image, output, d.size());
        applyMask(surface, l, d.size());
    }
    return surface;
}
static void folderMasks(QImage &surface, const Document &d, const Layer &l) {
    auto parent = l.parent();
    while (!parent.isEmpty()) {
        auto folder = d.find(parent);
        if (!folder)
            break;
        applyMask(surface, *folder, d.size());
        parent = folder->parent();
    }
}
static void fade(QImage &surface, double opacity) {
    for (int y = 0; y < surface.height(); ++y) {
        auto p = surface.scanLine(y);
        for (int x = 0; x < surface.width() * 4; ++x)
            p[x] = uchar(std::lround(p[x] * opacity));
    }
}
static QImage coverage(const Document &d, const Layer &l, QSize output, int depth = 0) {
    require(depth <= 256, "Clipping chain too deep");
    QImage surface;
    if (l.image.isNull()) {
        surface = QImage(output, QImage::Format_RGBA8888_Premultiplied);
        require(!surface.isNull(), "Not enough memory for clipping mask");
        surface.fill(Qt::transparent);
    } else
        surface = ownSurface(d, l, output);
    auto source = l.metadata.value("maskSourceID").toString();
    if (!source.isEmpty())
        if (auto base = d.find(normalizedId(source))) {
            auto mask = coverage(d, *base, output, depth + 1);
            QPainter p(&surface);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(0, 0, mask);
        }
    fade(surface, effectiveOpacity(d, l));
    return surface;
}
static QImage opaque(const QImage &source) {
    auto out = source;
    out.detach();
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) {
            auto p = out.scanLine(y) + x * 4;
            int a = p[3];
            for (int c = 0; c < 3; ++c)
                p[c] = a ? uchar(std::min(255, int(p[c]) * 255 / a)) : 0;
            p[3] = 255;
        }
    return out;
}
static void restoreAlpha(QImage &result, const QImage &original) {
    for (int y = 0; y < result.height(); ++y)
        for (int x = 0; x < result.width(); ++x) {
            auto p = result.scanLine(y) + x * 4;
            auto q = original.constScanLine(y) + x * 4;
            for (int c = 0; c < 3; ++c)
                p[c] = p[3] ? uchar(std::min(255, int(p[c]) * int(q[3]) / int(p[3]))) : 0;
            p[3] = q[3];
        }
}
static void adjustSurface(QImage &result, const Document &d, const Layer &l, bool folders) {
    auto a = l.metadata.value("adjustment").toObject();
    double scale = double(result.width()) / d.size().width();
    if (a.value("kind") == "Gaussian Blur")
        a["blurRadius"] = a.value("blurRadius").toDouble(10) * scale;
    if (a.value("kind") == "Motion Blur")
        a["motionDistance"] = std::max(1.0, a.value("motionDistance").toDouble(10) * scale);
    auto adjusted = applyAdjustment(result, a);
    if (l.blend() != "Normal") {
        auto blended = opaque(result);
        composite(blended, opaque(adjusted), l.blend(), 1);
        restoreAlpha(blended, result);
        adjusted = blended;
    }
    QImage mask(result.size(), QImage::Format_RGBA8888_Premultiplied);
    require(!mask.isNull(), "Not enough memory for adjustment");
    mask.fill(Qt::white);
    applyMask(mask, l, d.size());
    if (folders)
        folderMasks(mask, d, l);
    double opacity = effectiveOpacity(d, l);
    for (int y = 0; y < result.height(); ++y) {
        auto p = result.scanLine(y);
        auto q = adjusted.constScanLine(y);
        auto m = mask.constScanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            double amount = opacity * m[x * 4 + 3] / 255.0;
            for (int c = 0; c < 4; ++c)
                p[x * 4 + c] =
                    uchar(std::lround(p[x * 4 + c] * (1 - amount) + q[x * 4 + c] * amount));
        }
    }
}
QImage renderDocument(const Document &d, QSize output) {
    if (output.isEmpty())
        output = d.size();
    require(output.width() > 0 && output.height() > 0 &&
                qint64(output.width()) * output.height() <= MaxSurfacePixels,
            "Render exceeds 200 megapixel limit");
    QImage result(output, QImage::Format_RGBA8888_Premultiplied);
    require(!result.isNull(), "Not enough memory for canvas");
    result.fill(Qt::transparent);
    QVector<const Layer *> visibleLayers;
    std::function<void(const QString &, bool)> visit = [&](const QString &parent, bool visible) {
        for (const auto &l : d.layers) {
            if (l.parent() != parent)
                continue;
            bool shown = visible && l.visible();
            if (l.group()) {
                visit(l.id(), shown);
                continue;
            }
            if (shown)
                visibleLayers.push_back(&l);
        }
    };
    visit({}, true);
    for (qsizetype index = 0; index < visibleLayers.size(); ++index) {
        const auto &l = *visibleLayers[index];
        if (l.metadata.value("adjustment").isObject()) {
            if (l.metadata.value("maskSourceID").toString().isEmpty())
                adjustSurface(result, d, l, true);
            continue;
        }
        if (l.image.isNull())
            continue;
        auto surface = ownSurface(d, l, output);
        // Contiguous siblings clipped to one base share its alpha, including soft edges.
        qsizetype end = index + 1;
        if (l.metadata.value("maskSourceID").toString().isEmpty())
            while (end < visibleLayers.size() &&
                   normalizedId(visibleLayers[end]->metadata.value("maskSourceID").toString()) ==
                       l.id() &&
                   visibleLayers[end]->parent() == l.parent())
                ++end;
        if (end > index + 1) {
            fade(surface, effectiveOpacity(d, l));
            auto stack = opaque(surface);
            for (qsizetype child = index + 1; child < end; ++child) {
                const auto &c = *visibleLayers[child];
                if (c.metadata.value("adjustment").isObject())
                    adjustSurface(stack, d, c, false);
                else if (!c.image.isNull())
                    composite(stack, ownSurface(d, c, output), c.blend(), effectiveOpacity(d, c));
            }
            restoreAlpha(stack, surface);
            folderMasks(stack, d, l);
            composite(result, stack, l.blend(), 1);
            index = end - 1;
            continue;
        }
        auto source = l.metadata.value("maskSourceID").toString();
        if (!source.isEmpty())
            if (auto base = d.find(normalizedId(source))) {
                auto mask = coverage(d, *base, output);
                QPainter p(&surface);
                p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                p.drawImage(0, 0, mask);
            }
        folderMasks(surface, d, l);
        composite(result, surface, l.blend(), effectiveOpacity(d, l));
    }
    return result;
}
QImage layerSelection(const Document &d, const Layer &l, const QImage &selection) {
    if (selection.isNull())
        return {};
    QImage out(l.image.size(), QImage::Format_Grayscale8);
    require(!out.isNull(), "Not enough memory for selection");
    out.fill(0);
    auto transform = l.placement(l.image.size());
    for (int y = 0; y < out.height(); ++y) {
        auto p = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            auto q = transform.map(QPointF(x + 0.5, y + 0.5));
            int sx = int(std::floor(q.x())), sy = int(std::floor(q.y()));
            if (sx >= 0 && sy >= 0 && sx < d.size().width() && sy < d.size().height())
                p[x] = selection.constScanLine(sy)[sx];
        }
    }
    return out;
}
} // namespace compositor
