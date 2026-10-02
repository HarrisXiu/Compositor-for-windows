// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QString>

namespace compositor {
enum class BlendMode {
    Normal,
    Darken,
    Multiply,
    ColorBurn,
    LinearBurn,
    Lighten,
    Screen,
    ColorDodge,
    LinearDodge,
    Overlay,
    SoftLight,
    HardLight,
    VividLight,
    LinearLight,
    PinLight,
    HardMix,
    Difference,
    Exclusion,
    Subtract,
    Divide,
    Hue,
    Saturation,
    Color,
    Luminosity
};
BlendMode parseBlendMode(const QString &name);
// Both surfaces must have the same size and RGBA8888 premultiplied format.
void composite(QImage &back, const QImage &front, BlendMode mode, double opacity);
} // namespace compositor
