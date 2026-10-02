// SPDX-License-Identifier: MIT
#include "effects.h"
#include "document.h"
#include "filters.h"
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace compositor {
static double number(const QJsonObject &o, const char *key, double fallback = 0) {
    return o.value(QLatin1String(key)).toDouble(fallback);
}
void validateEffects(const QJsonObject &effects) {
    for (auto it = effects.begin(); it != effects.end(); ++it) {
        if (it.value().isNull())
            continue;
        require(
            QStringList{"stroke", "shadow", "colorOverlay", "innerShadow", "outerGlow", "innerGlow"}
                .contains(it.key()),
            "Unknown layer effect: " + it.key());
        require(it.value().isObject(), "Invalid layer effect");
        auto e = it.value().toObject();
        auto range = [&](const char *key, double low, double high) {
            if (!e.contains(QLatin1String(key)))
                return;
            auto v = number(e, key);
            require(e.value(QLatin1String(key)).isDouble() && std::isfinite(v) && v >= low &&
                        v <= high,
                    "Invalid effect parameter: " + QString::fromLatin1(key));
        };
        for (auto key : {"red", "green", "blue", "opacity"})
            range(key, 0, 1);
        range("size", 0, 500);
        range("blur", 0, 500);
        range("angle", -360, 360);
        range("distance", 0, 5000);
        require(!e.contains("enabled") || e.value("enabled").isBool(), "Invalid effect visibility");
    }
}
// Square dilation/erosion using two sliding-window passes, as in the Mac renderer.
static QImage extreme(const QImage &source, int reach, bool smallest) {
    auto sweep = [&](const QImage &input, QImage &output, bool vertical) {
        int lines = vertical ? input.width() : input.height();
        int count = vertical ? input.height() : input.width();
        std::vector<int> queue(count);
        for (int line = 0; line < lines; ++line) {
            auto read = [&](int i) {
                return vertical ? input.constScanLine(i)[line] : input.constScanLine(line)[i];
            };
            int head = 0, tail = 0, next = 0;
            for (int center = 0; center < count; ++center) {
                while (next <= std::min(count - 1, center + reach)) {
                    auto value = read(next);
                    while (tail > head && (smallest ? read(queue[tail - 1]) >= value
                                                    : read(queue[tail - 1]) <= value))
                        --tail;
                    queue[tail++] = next++;
                }
                while (head < tail && queue[head] < center - reach)
                    ++head;
                auto value = smallest && (center < reach || center + reach >= count)
                                 ? uchar(0)
                                 : read(queue[head]);
                if (vertical)
                    output.scanLine(center)[line] = value;
                else
                    output.scanLine(line)[center] = value;
            }
        }
    };
    QImage temp(source.size(), QImage::Format_Grayscale8),
        out(source.size(), QImage::Format_Grayscale8);
    require(!temp.isNull() && !out.isNull(), "Not enough memory for stroke");
    sweep(source, temp, false);
    sweep(temp, out, true);
    return out;
}
static QImage alpha(const QImage &image) {
    QImage out(image.size(), QImage::Format_Grayscale8);
    require(!out.isNull(), "Not enough memory for effects");
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x)
            out.scanLine(y)[x] = image.constScanLine(y)[x * 4 + 3];
    return out;
}
static QImage soften(const QImage &shape, double blur) {
    if (blur <= 0)
        return shape;
    return gaussianBlur(shape.convertToFormat(QImage::Format_RGBA8888_Premultiplied), blur / 2)
        .convertToFormat(QImage::Format_Grayscale8);
}
static QImage shifted(const QImage &shape, const QJsonObject &effect) {
    QImage out(shape.size(), QImage::Format_Grayscale8);
    out.fill(0);
    double angle = number(effect, "angle", 90) * std::numbers::pi / 180;
    double distance = number(effect, "distance", 20);
    // QPainter does not support grayscale painting; use an RGBA intermediate.
    QImage rgba(shape.size(), QImage::Format_RGBA8888_Premultiplied);
    rgba.fill(Qt::black);
    QPainter p(&rgba);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QPointF(-std::cos(angle) * distance, std::sin(angle) * distance), shape);
    p.end();
    return soften(rgba.convertToFormat(QImage::Format_Grayscale8), number(effect, "blur", 20));
}
static void fill(QImage &result, const QImage &coverage, const QJsonObject &e,
                 double defaultOpacity, double defaultColor = 0) {
    double color[3]{number(e, "red", defaultColor), number(e, "green", defaultColor),
                    number(e, "blue", defaultColor)};
    auto opacity = number(e, "opacity", defaultOpacity);
    for (int y = 0; y < result.height(); ++y) {
        auto p = result.scanLine(y);
        auto m = coverage.constScanLine(y);
        for (int x = 0; x < result.width(); ++x, p += 4) {
            double a = m[x] / 255.0 * opacity;
            for (int c = 0; c < 3; ++c)
                p[c] =
                    uchar(std::clamp(std::lround(color[c] * 255 * a + p[c] * (1 - a)), 0L, 255L));
            p[3] = uchar(std::clamp(std::lround(255 * a + p[3] * (1 - a)), 0L, 255L));
        }
    }
}
EffectImage renderEffects(const QImage &shown, const QJsonObject &effects) {
    validateEffects(effects);
    auto enabled = [&](const char *key) {
        auto e = effects.value(QLatin1String(key)).toObject();
        return !e.isEmpty() && e.value("enabled").toBool(true);
    };
    double margin = 0;
    auto stroke = effects.value("stroke").toObject(), shadow = effects.value("shadow").toObject(),
         outer = effects.value("outerGlow").toObject();
    if (enabled("stroke") && !stroke.value("inside").toBool())
        margin = number(stroke, "size", 4);
    if (enabled("shadow"))
        margin = std::max(margin, number(shadow, "distance", 20) + 3 * number(shadow, "blur", 20));
    if (enabled("outerGlow"))
        margin = std::max(margin, 3 * number(outer, "size", 20));
    int inset = int(std::ceil(margin)) + 2;
    QSize size = shown.size() + QSize(inset * 2, inset * 2);
    require(size.width() <= MaxSide && size.height() <= MaxSide &&
                qint64(size.width()) * size.height() <= MaxSurfacePixels,
            "Layer effects exceed surface limit");
    QImage pixels(size, QImage::Format_RGBA8888_Premultiplied), result(size, pixels.format());
    require(!pixels.isNull() && !result.isNull(), "Not enough memory for effects");
    pixels.fill(Qt::transparent);
    result.fill(Qt::transparent);
    {
        QPainter p(&pixels);
        p.drawImage(inset, inset, shown);
    }
    auto shape = alpha(pixels);
    if (enabled("shadow"))
        fill(result, shifted(shape, shadow), shadow, .5);
    auto insideCoverage = [&](QImage soft, bool inner) {
        for (int y = 0; y < soft.height(); ++y)
            for (int x = 0; x < soft.width(); ++x) {
                double s = shape.constScanLine(y)[x] / 255.0, b = soft.constScanLine(y)[x] / 255.0;
                soft.scanLine(y)[x] = uchar(std::lround(255 * (inner ? s * (1 - b) : b * (1 - s))));
            }
        return soft;
    };
    if (enabled("outerGlow"))
        fill(result, insideCoverage(soften(shape, number(outer, "size", 20)), false), outer, .75,
             1);
    auto drawStroke = [&] {
        bool inside = stroke.value("inside").toBool();
        auto ring =
            extreme(shape, std::max(1, int(std::lround(number(stroke, "size", 4)))), inside);
        for (int y = 0; y < ring.height(); ++y)
            for (int x = 0; x < ring.width(); ++x)
                ring.scanLine(y)[x] = uchar(std::max(
                    0, inside ? int(shape.constScanLine(y)[x]) - ring.constScanLine(y)[x]
                              : int(ring.constScanLine(y)[x]) - shape.constScanLine(y)[x]));
        fill(result, ring, stroke, 1);
    };
    if (enabled("stroke") && !stroke.value("inside").toBool() && number(stroke, "size", 4) > 0)
        drawStroke();
    {
        QPainter p(&result);
        p.drawImage(0, 0, pixels);
    }
    if (enabled("colorOverlay"))
        fill(result, shape, effects.value("colorOverlay").toObject(), 1);
    if (enabled("innerGlow")) {
        auto e = effects.value("innerGlow").toObject();
        fill(result, insideCoverage(soften(shape, number(e, "size", 10)), true), e, .75, 1);
    }
    if (enabled("innerShadow")) {
        auto e = effects.value("innerShadow").toObject();
        if (!e.contains("distance"))
            e["distance"] = 10;
        if (!e.contains("blur"))
            e["blur"] = 10;
        fill(result, insideCoverage(shifted(shape, e), true), e, .5);
    }
    if (enabled("stroke") && stroke.value("inside").toBool() && number(stroke, "size", 4) > 0)
        drawStroke();
    return {result, inset};
}
} // namespace compositor
