// SPDX-License-Identifier: MIT
#include "blend.h"
#include "document.h"
#include <QtConcurrent/QtConcurrentMap>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
namespace compositor {
BlendMode parseBlendMode(const QString &name) {
    if (name == "Normal")
        return BlendMode::Normal;
    if (name == "Darken")
        return BlendMode::Darken;
    if (name == "Multiply")
        return BlendMode::Multiply;
    if (name == "Color Burn")
        return BlendMode::ColorBurn;
    if (name == "Linear Burn")
        return BlendMode::LinearBurn;
    if (name == "Lighten")
        return BlendMode::Lighten;
    if (name == "Screen")
        return BlendMode::Screen;
    if (name == "Color Dodge")
        return BlendMode::ColorDodge;
    if (name == "Linear Dodge (Add)")
        return BlendMode::LinearDodge;
    if (name == "Overlay")
        return BlendMode::Overlay;
    if (name == "Soft Light")
        return BlendMode::SoftLight;
    if (name == "Hard Light")
        return BlendMode::HardLight;
    if (name == "Vivid Light")
        return BlendMode::VividLight;
    if (name == "Linear Light")
        return BlendMode::LinearLight;
    if (name == "Pin Light")
        return BlendMode::PinLight;
    if (name == "Hard Mix")
        return BlendMode::HardMix;
    if (name == "Difference")
        return BlendMode::Difference;
    if (name == "Exclusion")
        return BlendMode::Exclusion;
    if (name == "Subtract")
        return BlendMode::Subtract;
    if (name == "Divide")
        return BlendMode::Divide;
    if (name == "Hue")
        return BlendMode::Hue;
    if (name == "Saturation")
        return BlendMode::Saturation;
    if (name == "Color")
        return BlendMode::Color;
    if (name == "Luminosity")
        return BlendMode::Luminosity;
    return BlendMode::Normal;
}
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
static double channel(double b, double s, BlendMode m) {
    switch (m) {
    case BlendMode::Multiply:
        return b * s;
    case BlendMode::Screen:
        return b + s - b * s;
    case BlendMode::Darken:
        return std::min(b, s);
    case BlendMode::Lighten:
        return std::max(b, s);
    case BlendMode::ColorBurn:
        return b == 1 ? 1 : s == 0 ? 0 : 1 - std::min(1.0, (1 - b) / s);
    case BlendMode::ColorDodge:
        return b == 0 ? 0 : s == 1 ? 1 : std::min(1.0, b / (1 - s));
    case BlendMode::LinearBurn:
        return std::max(0.0, b + s - 1);
    case BlendMode::LinearDodge:
        return std::min(1.0, b + s);
    case BlendMode::Overlay:
        return b <= 0.5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case BlendMode::HardLight:
        return s <= 0.5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case BlendMode::SoftLight: {
        auto d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
        return s <= 0.5 ? b - (1 - 2 * s) * b * (1 - b) : b + (2 * s - 1) * (d - b);
    }
    case BlendMode::VividLight:
        return s <= 0.5 ? channel(b, 2 * s, BlendMode::ColorBurn)
                        : channel(b, 2 * s - 1, BlendMode::ColorDodge);
    case BlendMode::LinearLight:
        return std::clamp(b + 2 * s - 1, 0.0, 1.0);
    case BlendMode::PinLight:
        return s <= 0.5 ? std::min(b, 2 * s) : std::max(b, 2 * s - 1);
    case BlendMode::HardMix:
        return channel(b, s, BlendMode::VividLight) < 0.5 ? 0 : 1;
    case BlendMode::Difference:
        return std::abs(b - s);
    case BlendMode::Exclusion:
        return b + s - 2 * b * s;
    case BlendMode::Subtract:
        return std::max(0.0, b - s);
    case BlendMode::Divide:
        return b == 0 ? 0 : s == 0 ? 1 : std::min(1.0, b / s);
    default:
        return s;
    }
}
void composite(QImage &back, const QImage &front, BlendMode mode, double opacity) {
    require(back.size() == front.size() && back.format() == QImage::Format_RGBA8888_Premultiplied &&
                front.format() == QImage::Format_RGBA8888_Premultiplied,
            "Blend surfaces must have matching RGBA dimensions");
    require(std::isfinite(opacity) && opacity >= 0 && opacity <= 1, "Invalid blend opacity");
    if (back.isNull() || opacity == 0)
        return;
    // Detach once, before workers receive raw row pointers; never mutate QImage from workers.
    const QImage source = front;
    auto destination = back.bits();
    const auto input = source.constBits();
    const auto stride = back.bytesPerLine(), sourceStride = source.bytesPerLine();
    const int width = back.width();
    auto row = [&](int y) {
        auto b = destination + y * stride;
        auto s = input + y * sourceStride;
        for (int x = 0; x < width; ++x, b += 4, s += 4) {
            double sa = s[3] / 255.0 * opacity, ba = b[3] / 255.0;
            if (sa <= 0)
                continue;
            if (mode == BlendMode::Normal) {
                if (sa == 1) {
                    std::copy_n(s, 4, b);
                    continue;
                }
                for (int c = 0; c < 3; ++c) {
                    double sc = s[c] / double(s[3]);
                    double v = (1 - sa) * b[c] / 255.0 + (1 - ba) * sc * sa + sa * ba * sc;
                    b[c] = uchar(std::clamp(std::lround(v * 255), 0L, 255L));
                }
                b[3] = uchar(std::clamp(std::lround((sa + ba - sa * ba) * 255), 0L, 255L));
                continue;
            }
            Color bc{}, sc{}, blend{};
            for (int c = 0; c < 3; ++c) {
                bc[c] = ba > 0 ? b[c] / 255.0 / ba : 0;
                sc[c] = s[3] > 0 ? s[c] / double(s[3]) : 0;
            }
            if (mode == BlendMode::Hue)
                blend = setLum(setSat(sc, sat(bc)), lum(bc));
            else if (mode == BlendMode::Saturation)
                blend = setLum(setSat(bc, sat(sc)), lum(bc));
            else if (mode == BlendMode::Color)
                blend = setLum(sc, lum(bc));
            else if (mode == BlendMode::Luminosity)
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
    };
    if (qint64(width) * back.height() >= 256 * 256) {
        QVector<int> rows(back.height());
        std::iota(rows.begin(), rows.end(), 0);
        QtConcurrent::blockingMap(rows, row);
    } else {
        for (int y = 0; y < back.height(); ++y)
            row(y);
    }
}

} // namespace compositor
