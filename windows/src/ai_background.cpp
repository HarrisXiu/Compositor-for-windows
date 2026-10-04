// SPDX-License-Identifier: MIT
#include "ai_background.h"
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <vector>
namespace compositor {
namespace {
using Plane = std::vector<float>;
void check(const std::shared_ptr<AiCancellation> &cancel) {
    if (cancel)
        cancel->check();
}
uchar byte(double value) {
    return uchar(std::clamp(std::lround(value * 255), 0L, 255L));
}
Plane box(const Plane &source, int w, int h, int radius,
          const std::shared_ptr<AiCancellation> &cancel) {
    Plane pass(source.size()), out(source.size());
    const double span = radius * 2 + 1;
    for (int y = 0; y < h; ++y) {
        check(cancel);
        const auto row = size_t(y) * w;
        double sum = 0;
        for (int x = -radius; x <= radius; ++x)
            sum += source[row + std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            pass[row + x] = float(sum / span);
            sum += source[row + std::clamp(x + radius + 1, 0, w - 1)] -
                   source[row + std::clamp(x - radius, 0, w - 1)];
        }
    }
    for (int x = 0; x < w; ++x) {
        check(cancel);
        double sum = 0;
        for (int y = -radius; y <= radius; ++y)
            sum += pass[size_t(std::clamp(y, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            out[size_t(y) * w + x] = float(sum / span);
            sum += pass[size_t(std::clamp(y + radius + 1, 0, h - 1)) * w + x] -
                   pass[size_t(std::clamp(y - radius, 0, h - 1)) * w + x];
        }
    }
    return out;
}
// The two box stages need a 2r halo. Tiling keeps temporary float planes bounded,
// while every emitted pixel uses the same neighborhood as a whole-image filter.
QImage guided(const QImage &mask, const QImage &guide, int radius,
              const std::shared_ptr<AiCancellation> &cancel) {
    QImage out(mask.size(), QImage::Format_Grayscale8);
    require(!out.isNull(), "Not enough memory for background refinement");
    constexpr int tile = 512;
    for (int top = 0; top < mask.height(); top += tile)
        for (int left = 0; left < mask.width(); left += tile) {
            check(cancel);
            const QRect target(left, top, std::min(tile, mask.width() - left),
                               std::min(tile, mask.height() - top));
            const auto area = target.adjusted(-2 * radius, -2 * radius, 2 * radius, 2 * radius)
                                  .intersected(mask.rect());
            const auto rgb = guide.copy(area).convertToFormat(QImage::Format_RGBA8888);
            require(!rgb.isNull(), "Not enough memory for background refinement");
            const int w = area.width(), h = area.height();
            const size_t count = size_t(w) * h;
            Plane g(count), p(count), squares(count), products(count);
            for (int y = 0; y < h; ++y) {
                check(cancel);
                auto source = rgb.constScanLine(y);
                auto matte = mask.constScanLine(area.y() + y) + area.x();
                for (int x = 0; x < w; ++x) {
                    const auto i = size_t(y) * w + x;
                    g[i] = float((.2126 * source[4 * x] + .7152 * source[4 * x + 1] +
                                  .0722 * source[4 * x + 2]) /
                                 255);
                    p[i] = matte[x] / 255.f;
                    squares[i] = g[i] * g[i];
                    products[i] = g[i] * p[i];
                }
            }
            auto mg = box(g, w, h, radius, cancel), mp = box(p, w, h, radius, cancel);
            auto ms = box(squares, w, h, radius, cancel), mc = box(products, w, h, radius, cancel);
            for (size_t i = 0; i < count; ++i) {
                squares[i] =
                    (mc[i] - mg[i] * mp[i]) / (std::max(0.f, ms[i] - mg[i] * mg[i]) + 1e-4f);
                products[i] = mp[i] - squares[i] * mg[i];
            }
            const auto a = box(squares, w, h, radius, cancel),
                       b = box(products, w, h, radius, cancel);
            for (int y = target.top(); y <= target.bottom(); ++y) {
                check(cancel);
                auto row = out.scanLine(y);
                for (int x = target.left(); x <= target.right(); ++x) {
                    const auto i = size_t(y - area.y()) * w + x - area.x();
                    row[x] = byte(std::clamp(double(a[i] * g[i] + b[i]), 0., 1.));
                }
            }
        }
    return out;
}
QImage shiftEdge(const QImage &mask, int shift, const std::shared_ptr<AiCancellation> &cancel) {
    const double sigma = std::abs(shift) / 2.;
    const int radius = int(std::ceil(3 * sigma));
    std::vector<double> weights(size_t(2 * radius + 1));
    double sum = 0;
    for (int i = -radius; i <= radius; ++i) {
        weights[i + radius] = std::exp(-i * i / (2 * sigma * sigma));
        sum += weights[i + radius];
    }
    for (auto &v : weights)
        v /= sum;
    QImage out(mask.size(), QImage::Format_Grayscale8);
    require(!out.isNull(), "Not enough memory for background refinement");
    constexpr int tile = 512;
    for (int top = 0; top < mask.height(); top += tile)
        for (int left = 0; left < mask.width(); left += tile) {
            check(cancel);
            const QRect target(left, top, std::min(tile, mask.width() - left),
                               std::min(tile, mask.height() - top));
            const auto area =
                target.adjusted(-radius, -radius, radius, radius).intersected(mask.rect());
            Plane pass(size_t(area.width()) * area.height());
            for (int y = 0; y < area.height(); ++y) {
                check(cancel);
                const auto row = mask.constScanLine(area.y() + y);
                for (int x = 0; x < area.width(); ++x) {
                    double value = 0;
                    for (int k = -radius; k <= radius; ++k)
                        value += weights[k + radius] *
                                 row[std::clamp(area.x() + x + k, 0, mask.width() - 1)] / 255.;
                    pass[size_t(y) * area.width() + x] = float(value);
                }
            }
            const double level = shift < 0 ? .75 : .25;
            for (int y = target.top(); y <= target.bottom(); ++y) {
                check(cancel);
                auto row = out.scanLine(y);
                for (int x = target.left(); x <= target.right(); ++x) {
                    double value = 0;
                    for (int k = -radius; k <= radius; ++k)
                        value += weights[k + radius] *
                                 pass[size_t(std::clamp(y + k, 0, mask.height() - 1) - area.y()) *
                                          area.width() +
                                      x - area.x()];
                    row[x] = byte(std::clamp((value - level) / .001, 0., 1.));
                }
            }
        }
    return out;
}
} // namespace
QImage refineBackgroundMatte(const QImage &mask, const QImage &guide, BackgroundSettings settings,
                             std::shared_ptr<AiCancellation> cancel) {
    require(!guide.isNull() && mask.size() == guide.size() &&
                mask.format() == QImage::Format_Grayscale8,
            "Invalid background matte");
    require(settings.refine >= 0 && settings.refine <= 40 && settings.contrast >= 0 &&
                settings.contrast <= 100 && settings.shift >= -10 && settings.shift <= 10,
            "Invalid background settings");
    check(cancel);
    if (!settings.advanced)
        return mask;
    auto out = settings.refine ? guided(mask, guide, settings.refine, cancel) : mask;
    if (settings.shift)
        out = shiftEdge(out, settings.shift, cancel);
    if (settings.contrast) {
        const double slope = 1 / std::max(.02, 1 - settings.contrast / 100. * .98);
        for (int y = 0; y < out.height(); ++y) {
            check(cancel);
            auto row = out.scanLine(y);
            for (int x = 0; x < out.width(); ++x)
                row[x] = byte(std::clamp((row[x] / 255. - .5) * slope + .5, 0., 1.));
        }
    }
    check(cancel);
    return out;
}
QImage backgroundLayerMask(const Document &document, const Layer &layer, const QImage &matte,
                           const QImage &selection, std::shared_ptr<AiCancellation> cancel) {
    require(!layer.image.isNull() && matte.size() == layer.image.size() &&
                matte.format() == QImage::Format_Grayscale8,
            "Invalid background layer matte");
    require(selection.isNull() || (selection.size() == document.size() &&
                                   selection.format() == QImage::Format_Grayscale8),
            "Invalid background selection");
    check(cancel);
    QImage existing;
    if (!layer.mask.isNull()) {
        QImage coverage(layer.mask.size(), QImage::Format_ARGB32_Premultiplied);
        require(!coverage.isNull(), "Not enough memory for background mask");
        const auto gray = layer.mask.convertToFormat(QImage::Format_Grayscale8);
        for (int y = 0; y < gray.height(); ++y) {
            check(cancel);
            auto row = reinterpret_cast<QRgb *>(coverage.scanLine(y));
            const auto src = gray.constScanLine(y);
            for (int x = 0; x < gray.width(); ++x)
                row[x] = qRgba(src[x], src[x], src[x], src[x]);
        }
        existing = QImage(layer.image.size(), QImage::Format_ARGB32_Premultiplied);
        require(!existing.isNull(), "Not enough memory for background mask");
        existing.fill(Qt::transparent);
        auto placed = layer;
        if (layer.metadata.value("maskPlacement").isObject())
            placed.metadata["transform"] = layer.metadata.value("maskPlacement");
        bool invertible = false;
        const auto inverse = layer.placement(layer.image.size()).inverted(&invertible);
        require(invertible, "Cannot remove background from a singular layer transform");
        QPainter painter(&existing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setTransform(placed.placement(layer.mask.size()) * inverse);
        painter.drawImage(0, 0, coverage);
        painter.end();
    }
    auto out = matte;
    out.detach();
    const auto mapping = layer.placement(layer.image.size());
    for (int y = 0; y < out.height(); ++y) {
        check(cancel);
        auto row = out.scanLine(y);
        const auto old =
            existing.isNull() ? nullptr : reinterpret_cast<const QRgb *>(existing.constScanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            int selected = 255;
            if (!selection.isNull()) {
                const auto point = mapping.map(QPointF(x + .5, y + .5));
                const int sx = int(std::floor(point.x())), sy = int(std::floor(point.y()));
                selected = selection.valid(sx, sy) ? selection.constScanLine(sy)[sx] : 0;
            }
            const int base = old ? qAlpha(old[x]) : 255;
            row[x] = uchar((base * (255 * (255 - selected) + row[x] * selected) + 32512) / 65025);
        }
    }
    check(cancel);
    return out;
}
QImage backgroundCutout(const QImage &image, const QImage &mask,
                        std::shared_ptr<AiCancellation> cancel) {
    require(!image.isNull() && image.size() == mask.size() &&
                mask.format() == QImage::Format_Grayscale8,
            "Invalid background preview mask");
    auto out = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    require(!out.isNull(), "Not enough memory for background preview");
    for (int y = 0; y < out.height(); ++y) {
        check(cancel);
        auto row = out.scanLine(y);
        const auto matte = mask.constScanLine(y);
        for (int x = 0; x < out.width(); ++x)
            for (int c = 0; c < 4; ++c)
                row[4 * x + c] = uchar((row[4 * x + c] * matte[x] + 127) / 255);
    }
    check(cancel);
    return out;
}
void installBackgroundMask(Layer &layer, const QImage &mask) {
    require(mask.size() == layer.image.size() && mask.format() == QImage::Format_Grayscale8,
            "Invalid background layer mask");
    // Bake any existing placement into the layer grid; the new mask follows its pixels.
    layer.mask = mask;
    layer.metadata["maskFile"] = layer.id() + ".mask.png";
    layer.metadata.remove("maskPlacement");
    layer.metadata["maskLinked"] = true;
    layer.metadata["maskEnabled"] = true;
}
} // namespace compositor
