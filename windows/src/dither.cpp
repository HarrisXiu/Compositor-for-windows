// SPDX-License-Identifier: MIT
#include "dither.h"
#include "DitherPixels.h"
#include "document.h"
#include "filters.h"
#include <QColorSpace>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <vector>
namespace compositor {
QStringList ditherStyles() {
    return {"Atkinson (Classic Mac)",
            "Floyd–Steinberg",
            "Bayer 2 × 2",
            "Bayer 4 × 4",
            "Bayer 8 × 8",
            "Halftone Dots",
            "Halftone Lines",
            "Halftone Diamonds",
            "Mac Patterns",
            "ASCII",
            "Scanlines (CRT)"};
}
namespace {
double number(const QJsonObject &s, const char *key, double value, double min, double max) {
    double v = s.value(QLatin1String(key)).toDouble(value);
    require(std::isfinite(v), "Invalid dither parameter");
    return std::clamp(v, min, max);
}
struct Glyphs {
    std::vector<uchar> maps;
    std::vector<float> coverage;
    int width = 1, height = 1;
};
Glyphs glyphs(QString characters, int height) {
    require(qobject_cast<QGuiApplication *>(QCoreApplication::instance()),
            "ASCII dithering requires the graphics application");
    QFont font("Consolas");
    font.setPixelSize(int(std::lround(height / 1.2)));
    font.setBold(true);
    font.setStyleHint(QFont::Monospace);
    QFontMetricsF metrics(font);
    Glyphs out;
    out.width = std::max(1, int(std::lround(metrics.horizontalAdvance("M"))));
    out.height = height;
    struct Glyph {
        QByteArray map;
        float mean;
    };
    std::vector<Glyph> drawn;
    QSet<char32_t> seen;
    auto points = characters.toUcs4();
    for (auto point : points) {
        char32_t character = char32_t(point);
        if (seen.contains(character) || character == '\r' || character == '\n')
            continue;
        seen.insert(character);
        if (seen.size() > 64)
            break;
        QString text = QString::fromUcs4(&character, 1);
        QImage map(out.width, out.height, QImage::Format_ARGB32_Premultiplied);
        map.fill(Qt::black);
        QPainter p(&map);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(font);
        p.setPen(Qt::white);
        double baseline = std::round((height - metrics.height()) / 2 + metrics.ascent());
        p.drawText(QPointF(std::round((out.width - metrics.horizontalAdvance(text)) / 2), baseline),
                   text);
        p.end();
        Glyph g;
        g.map.resize(out.width * out.height);
        int sum = 0;
        for (int y = 0; y < out.height; ++y)
            for (int x = 0; x < out.width; ++x) {
                uchar v = uchar(qGray(map.pixel(x, y)));
                g.map[y * out.width + x] = char(v);
                sum += v;
            }
        g.mean = float(sum) / float(255 * out.width * out.height);
        drawn.push_back(std::move(g));
    }
    std::stable_sort(drawn.begin(), drawn.end(),
                     [](const auto &a, const auto &b) { return a.mean < b.mean; });
    for (auto &g : drawn) {
        out.maps.insert(out.maps.end(), g.map.begin(), g.map.end());
        out.coverage.push_back(g.mean);
    }
    return out;
}
} // namespace
QImage applyDither(const QImage &source, const QJsonObject &s) {
    require(!source.isNull(), "Select an image to dither");
    int style = ditherStyles().indexOf(s.value("style").toString(ditherStyles().first()));
    require(style >= 0, "Unknown dither style");
    int block = style == DITHER_GLYPHS || style == DITHER_SCANLINES
                    ? 1
                    : int(std::lround(number(s, "pixelSize", 2, 1, 32)));
    auto image = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (block > 1) {
        QImage small((image.width() + block - 1) / block, (image.height() + block - 1) / block,
                     image.format());
        require(!small.isNull(), "Not enough memory for dither pixels");
        for (int y = 0; y < small.height(); ++y)
            for (int x = 0; x < small.width(); ++x) {
                int sum[4]{}, count = 0;
                for (int yy = y * block; yy < std::min((y + 1) * block, image.height()); ++yy)
                    for (int xx = x * block; xx < std::min((x + 1) * block, image.width()); ++xx) {
                        auto p = image.constScanLine(yy) + xx * 4;
                        for (int c = 0; c < 4; ++c)
                            sum[c] += p[c];
                        ++count;
                    }
                auto q = small.scanLine(y) + x * 4;
                for (int c = 0; c < 4; ++c)
                    q[c] = uchar((sum[c] + count / 2) / count);
            }
        image = std::move(small);
    }
    DitherParams p{};
    p.style = style;
    p.levels = int(std::lround(number(s, "levels", 2, 2, 8)));
    p.diffusion = float(number(s, "diffusion", 100, 0, 100) / 100);
    p.density = float(number(s, "density", 0, -100, 100) / 100);
    p.contrast = float(number(s, "contrast", 0, -100, 100) / 100);
    p.cell = int(std::lround(style == DITHER_SCANLINES ? number(s, "lineSpacing", 4, 2, 32)
                                                       : number(s, "cellSize", 8, 4, 64)));
    p.angle = float(number(s, "angle", 45, -90, 90) * 3.141592653589793 / 180);
    p.lightOnDark = s.value("lightOnDark").toBool(true);
    p.originalColors = s.value("colors").toString() == "Original";
    const char *keys[3] = {"red", "green", "blue"};
    for (int c = 0; c < 3; ++c) {
        p.dark[c] = 0;
        p.light[c] = 255;
        if (s.value("colors").toString() == "Two Colors") {
            p.dark[c] =
                uchar(std::lround(number(s.value("dark").toObject(), keys[c], 0, 0, 1) * 255));
            p.light[c] =
                uchar(std::lround(number(s.value("light").toObject(), keys[c], 1, 0, 1) * 255));
        }
    }
    p.dots = float(number(s, "dots", 0, 0, 100) / 100);
    p.wobble = float(number(s, "wobble", 0, 0, 64));
    Glyphs atlas;
    if (style == DITHER_GLYPHS) {
        auto chars = s.value("characters").toString();
        chars.remove('\r');
        chars.remove('\n');
        if (chars.isEmpty())
            chars = " .:-=+*#%@";
        atlas = glyphs(chars, int(std::lround(number(s, "textSize", 14, 6, 64))));
        p.glyphWidth = atlas.width;
        p.glyphHeight = atlas.height;
        p.glyphCount = int(atlas.coverage.size());
        p.glyphs = atlas.maps.data();
        p.glyphCoverage = atlas.coverage.data();
    }
    image.detach();
    require(dither_apply(image.bits(), size_t(image.width()), size_t(image.height()),
                         size_t(image.bytesPerLine()), &p) == 1,
            "Not enough memory for dithering");
    if (style == DITHER_SCANLINES && number(s, "glow", 35, 0, 100) > 0) {
        double sigma = p.cell * 3 + 3, shrink = std::max(1.0, std::floor(sigma / 4));
        auto small = image.scaled(std::max(1, int(std::ceil(image.width() / shrink))),
                                  std::max(1, int(std::ceil(image.height() / shrink))),
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        auto bloom = gaussianBlur(small, sigma / shrink, true)
                         .scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                         .convertToFormat(image.format());
        dither_glow(image.bits(), bloom.constBits(), size_t(image.width()), size_t(image.height()),
                    size_t(image.bytesPerLine()), float(number(s, "glow", 35, 0, 100) / 100 * 2.5));
    }
    if (block > 1) {
        QImage full(source.size(), image.format());
        require(!full.isNull(), "Not enough memory for dither result");
        full.fill(Qt::transparent);
        full.setColorSpace(source.colorSpace());
        QPainter painter(&full);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(QRect(0, 0, image.width() * block, image.height() * block), image);
        painter.end();
        image = std::move(full);
        if (s.value("pixelShape").toString() == "Dot")
            dither_dots(image.bits(), size_t(image.width()), size_t(image.height()),
                        size_t(image.bytesPerLine()), block, p.dark);
    }
    return image;
}
} // namespace compositor
