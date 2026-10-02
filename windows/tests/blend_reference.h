// SPDX-License-Identifier: MIT
// Frozen 0.3 scalar compositor for pixel-equivalence regression tests.
#pragma once
#include <QImage>
#include <QString>
#include <algorithm>
#include <array>
#include <cmath>
namespace blend_reference {
using Color = std::array<double, 3>;
inline double lum(Color c) {
    return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
}
inline Color setLum(Color c, double l) {
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
inline double sat(Color c) {
    return *std::max_element(c.begin(), c.end()) - *std::min_element(c.begin(), c.end());
}
inline Color setSat(Color c, double s) {
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
inline double channel(double b, double s, const QString &m) {
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
inline void referenceComposite(QImage &back, const QImage &front, const QString &mode,
                               double opacity) {
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
} // namespace blend_reference
