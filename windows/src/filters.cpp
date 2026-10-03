// SPDX-License-Identifier: MIT
#include "filters.h"
#include "camera_raw.h"
#include "dither.h"
#include "document.h"
#include <QColor>
#include <QJsonArray>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>
extern "C" {
#include "AdjustPixels.h"
#include "ContentFill.h"
#include "HealPixels.h"
#include "LensPixels.h"
#include "LevelsPixels.h"
#include "NoisePixels.h"
}
namespace compositor {
static double num(const QJsonObject &o, const char *key, double fallback = 0) {
    return o.value(QLatin1String(key)).toDouble(fallback);
}
static uchar byte(double v) {
    return uchar(std::clamp(std::lround(v), 0L, 255L));
}
static QJsonObject keyedDictionary(const QJsonValue &v) {
    if (v.isObject())
        return v.toObject();
    // Swift Codable dictionaries with enum keys use alternating key/value arrays.
    auto a = v.toArray();
    QJsonObject out;
    for (int i = 0; i + 1 < a.size(); i += 2)
        out[a[i].toString()] = a[i + 1];
    return out;
}
double curveValue(double x, const QJsonArray &points) {
    if (points.isEmpty())
        return x;
    require(points.size() >= 2 && points.size() <= 64, "Invalid curve points");
    std::vector<double> px, py, d, slopes;
    for (const auto &v : points) {
        auto p = v.toObject();
        double cx = num(p, "x"), cy = num(p, "y");
        require(std::isfinite(cx) && std::isfinite(cy) && cx >= 0 && cx <= 255 && cy >= 0 &&
                    cy <= 255 && (px.empty() || cx > px.back()),
                "Invalid curve point");
        px.push_back(cx);
        py.push_back(cy);
    }
    require(px.front() == 0 && px.back() == 255, "Curve endpoints must span 0 to 255");
    for (size_t i = 1; i < px.size(); ++i)
        d.push_back((py[i] - py[i - 1]) / (px[i] - px[i - 1]));
    slopes.push_back(d.front());
    for (size_t i = 1; i < d.size(); ++i)
        slopes.push_back(d[i - 1] * d[i] <= 0 ? 0 : 2 / (1 / d[i - 1] + 1 / d[i]));
    slopes.push_back(d.back());
    size_t i = 0;
    while (i + 2 < px.size() && x > px[i + 1])
        ++i;
    double h = px[i + 1] - px[i], t = std::clamp((x - px[i]) / h, 0.0, 1.0), t2 = t * t,
           t3 = t2 * t;
    return std::clamp((2 * t3 - 3 * t2 + 1) * py[i] + (t3 - 2 * t2 + t) * h * slopes[i] +
                          (-2 * t3 + 3 * t2) * py[i + 1] + (t3 - t2) * h * slopes[i + 1],
                      0.0, 255.0);
}
static void applyHue(uchar *pixels, size_t count, QJsonObject s) {
    auto adjustments = keyedDictionary(s.value("adjustments")),
         bands = keyedDictionary(s.value("bands"));
    if (!s.contains("adjustments"))
        adjustments["Master"] = QJsonObject{{"hue", s.value("hue")},
                                            {"saturation", s.value("saturation")},
                                            {"lightness", s.value("lightness")}};
    const QStringList names{"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
    const double starts[]{0, 315, 15, 75, 135, 195, 255};
    auto forward = [](double from, double to) {
        double d = std::fmod(to - from, 360);
        return d < 0 ? d + 360 : d;
    };
    std::array<std::array<double, 3>, 361> response{};
    for (int h = 0; h <= 360; ++h)
        for (int r = 0; r < names.size(); ++r) {
            auto a = adjustments.value(names[r]).toObject();
            double weight = 1;
            if (r > 0) {
                auto b = bands.value(names[r]).toObject();
                double start = num(b, "falloffStart", starts[r]),
                       end = num(b, "falloffEnd", std::fmod(starts[r] + 90, 360));
                double span = forward(start, end), pos = forward(start, h),
                       ramp = forward(start, num(b, "rangeStart", std::fmod(starts[r] + 30, 360))),
                       plateau = forward(start, num(b, "rangeEnd", std::fmod(starts[r] + 60, 360)));
                weight = span <= 0        ? 1
                         : pos > span     ? 0
                         : pos < ramp     ? pos / ramp
                         : pos <= plateau ? 1
                         : span > plateau ? (span - pos) / (span - plateau)
                                          : 1;
                if (s.value("invertRange").toBool() && s.value("range").toString() == names[r])
                    weight = 1 - weight;
            }
            const char *keys[]{"hue", "saturation", "lightness"};
            for (int c = 0; c < 3; ++c)
                response[h][c] += num(a, keys[c]) * weight;
        }
    constexpr int dimension = 33;
    std::vector<float> cube(dimension * dimension * dimension * 4);
    size_t index = 0;
    for (int b = 0; b < dimension; ++b)
        for (int g = 0; g < dimension; ++g)
            for (int r = 0; r < dimension; ++r) {
                double red = r / 32.0, green = g / 32.0, blue = b / 32.0;
                double hi = std::max({red, green, blue}), lo = std::min({red, green, blue}),
                       delta = hi - lo;
                double l = (hi + lo) / 2,
                       saturation = delta == 0 ? 0 : delta / (1 - std::abs(2 * l - 1)), h = 0;
                if (delta > 0)
                    h = 60 * (hi == red     ? std::fmod((green - blue) / delta, 6)
                              : hi == green ? (blue - red) / delta + 2
                                            : (red - green) / delta + 4);
                if (h < 0)
                    h += 360;
                auto sampled = response[std::clamp(int(std::lround(h)), 0, 360)];
                double lightness = sampled[2] / 100;
                if (s.value("colorize").toBool()) {
                    auto a = adjustments.value(s.value("range").toString("Master")).toObject();
                    h = num(a, "hue");
                    saturation = std::clamp(num(a, "saturation") / 100, 0.0, 1.0);
                    lightness = num(a, "lightness") / 100;
                } else {
                    h = std::fmod(h + sampled[0] + 720, 360);
                    double amount = std::clamp(sampled[1] / 100, -1.0, 1.0);
                    saturation = amount <= 0   ? saturation * (1 + amount)
                                 : amount >= 1 ? (saturation > 0 ? 1 : 0)
                                               : std::min(1.0, saturation / (1 - amount));
                }
                lightness = std::clamp(lightness, -1.0, 1.0);
                l = lightness >= 0 ? l + (1 - l) * lightness : l * (1 + lightness);
                h = std::fmod(h + 720, 360);
                double chroma = (1 - std::abs(2 * l - 1)) * saturation,
                       second = chroma * (1 - std::abs(std::fmod(h / 60, 2) - 1)),
                       base = l - chroma / 2;
                double rgb[3]{};
                if (h < 60) {
                    rgb[0] = chroma;
                    rgb[1] = second;
                } else if (h < 120) {
                    rgb[0] = second;
                    rgb[1] = chroma;
                } else if (h < 180) {
                    rgb[1] = chroma;
                    rgb[2] = second;
                } else if (h < 240) {
                    rgb[1] = second;
                    rgb[2] = chroma;
                } else if (h < 300) {
                    rgb[0] = second;
                    rgb[2] = chroma;
                } else {
                    rgb[0] = chroma;
                    rgb[2] = second;
                }
                for (int c = 0; c < 3; ++c)
                    cube[index++] = float(std::clamp(rgb[c] + base, 0.0, 1.0));
                cube[index++] = 1;
            }
    cube_apply(pixels, count, cube.data(), dimension);
}
static QImage motionBlur(const QImage &source, double angle, double distance) {
    require(std::isfinite(angle) && std::isfinite(distance) && distance >= 1 && distance <= 2000,
            "Invalid motion blur");
    QImage out(source.size(), QImage::Format_RGBA8888_Premultiplied);
    require(!out.isNull(), "Not enough memory for motion blur");
    double radians = angle * std::numbers::pi / 180, dx = std::cos(radians),
           dy = -std::sin(radians);
    int samples = std::max(2, int(std::ceil(distance)) + 1);
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) {
            double value[4]{};
            for (int i = 0; i < samples; ++i) {
                double offset = (double(i) / (samples - 1) - .5) * distance, sx = x + dx * offset,
                       sy = y + dy * offset;
                int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                double fx = sx - ix, fy = sy - iy;
                for (int j = 0; j < 2; ++j)
                    for (int k = 0; k < 2; ++k)
                        if (ix + k >= 0 && ix + k < source.width() && iy + j >= 0 &&
                            iy + j < source.height()) {
                            auto p = source.constScanLine(iy + j) + (ix + k) * 4;
                            double w = (k ? fx : 1 - fx) * (j ? fy : 1 - fy);
                            for (int c = 0; c < 4; ++c)
                                value[c] += p[c] * w;
                        }
            }
            auto p = out.scanLine(y) + x * 4;
            for (int c = 0; c < 4; ++c)
                p[c] = byte(value[c] / samples);
        }
    return out;
}
QImage gaussianBlur(const QImage &source, double sigma, bool clampEdges) {
    if (sigma <= 0)
        return source;
    require(std::isfinite(sigma) && sigma <= 250, "Invalid blur radius");
    QImage input = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QImage temp(input.size(), input.format()), out(input.size(), input.format());
    require(!temp.isNull() && !out.isNull(), "Not enough memory for blur");
    const int radius = int(std::ceil(3 * sigma));
    std::vector<double> weights(2 * radius + 1);
    double sum = 0;
    for (int i = -radius; i <= radius; ++i) {
        weights[i + radius] = std::exp(-double(i * i) / (2 * sigma * sigma));
        sum += weights[i + radius];
    }
    for (auto &v : weights)
        v /= sum;
    for (int y = 0; y < input.height(); ++y)
        for (int x = 0; x < input.width(); ++x) {
            double value[4]{};
            for (int i = -radius; i <= radius; ++i) {
                int sx = x + i;
                if (clampEdges)
                    sx = std::clamp(sx, 0, input.width() - 1);
                if (sx < 0 || sx >= input.width())
                    continue;
                auto p = input.constScanLine(y) + sx * 4;
                for (int c = 0; c < 4; ++c)
                    value[c] += p[c] * weights[i + radius];
            }
            auto p = temp.scanLine(y) + x * 4;
            for (int c = 0; c < 4; ++c)
                p[c] = byte(value[c]);
        }
    for (int y = 0; y < input.height(); ++y)
        for (int x = 0; x < input.width(); ++x) {
            double value[4]{};
            for (int i = -radius; i <= radius; ++i) {
                int sy = y + i;
                if (clampEdges)
                    sy = std::clamp(sy, 0, input.height() - 1);
                if (sy < 0 || sy >= input.height())
                    continue;
                auto p = temp.constScanLine(sy) + x * 4;
                for (int c = 0; c < 4; ++c)
                    value[c] += p[c] * weights[i + radius];
            }
            auto p = out.scanLine(y) + x * 4;
            for (int c = 0; c < 4; ++c)
                p[c] = byte(value[c]);
        }
    return out;
}
QImage applyFilter(const QImage &source, const QString &kind, const QJsonObject &s) {
    require(!source.isNull(), "Select a pixel layer");
    if (kind == "Dither")
        return applyDither(source, s);
    auto out = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (kind == "Camera Raw")
        out = cameraRawGeometry(out, s);
    out.detach();
    auto p = out.bits();
    const auto width = size_t(out.width()), height = size_t(out.height()),
               stride = size_t(out.bytesPerLine());
    if (kind == "Invert") {
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x) {
                auto q = out.scanLine(y) + x * 4;
                for (int c = 0; c < 3; ++c)
                    q[c] = q[3] - q[c];
            }
    } else if (kind == "Exposure") {
        float tables[768];
        const auto scale = std::pow(2.0, num(s, "exposure"));
        const auto gamma = std::max(0.01, num(s, "gamma", 1));
        for (int i = 0; i < 256; ++i) {
            double v = i / 255.0;
            v = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
            v = std::pow(std::max(0.0, v * scale + num(s, "offset")), 1 / gamma);
            v = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
            for (int c = 0; c < 3; ++c)
                tables[c * 256 + i] = float(std::clamp(v, 0.0, 1.0));
        }
        levels_apply(p, width * height, tables);
    } else if (kind == "Levels") {
        float tables[768];
        auto channels = s.value("ranges").toArray();
        auto map = [](double v, const QJsonObject &r) {
            auto low = num(r, "black", num(r, "inputBlack")),
                 high = num(r, "white", num(r, "inputWhite", 255));
            auto gamma = std::max(0.1, num(r, "gamma", 1));
            auto t = std::pow(std::clamp((v * 255 - low) / std::max(1.0, high - low), 0.0, 1.0),
                              1 / gamma);
            return (num(r, "outputBlack") +
                    (num(r, "outputWhite", 255) - num(r, "outputBlack")) * t) /
                   255;
        };
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) {
                double v = i / 255.0;
                if (channels.size() == 4) {
                    v = map(v, channels[c + 1].toObject());
                    v = map(v, channels[0].toObject());
                } else
                    v = map(v, s);
                tables[c * 256 + i] = float(v);
            }
        levels_apply(p, width * height, tables);
    } else if (kind == "Curves") {
        float tables[768];
        auto channels = s.value("channels").toArray();
        require(channels.isEmpty() || channels.size() == 4, "Invalid curve channels");
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) {
                double v =
                    curveValue(i, channels.size() == 4 ? channels[c + 1].toArray() : QJsonArray());
                tables[c * 256 + i] =
                    float(curveValue(v, channels.size() == 4 ? channels[0].toArray()
                                                             : s.value("points").toArray()) /
                          255);
            }
        levels_apply(p, width * height, tables);
    } else if (kind == "Hue/Saturation") {
        applyHue(p, width * height, s);
    } else if (kind == "Black & White") {
        const float weights[6] = {
            float(num(s, "reds", 40) / 100),   float(num(s, "yellows", 60) / 100),
            float(num(s, "greens", 40) / 100), float(num(s, "cyans", 60) / 100),
            float(num(s, "blues", 20) / 100),  float(num(s, "magentas", 80) / 100)};
        adjust_black_white(p, width, height, stride, weights, s.value("tint").toBool(),
                           num(s, "tintHue", 40), num(s, "tintSaturation", 20) / 100);
    } else if (kind == "Add Noise") {
        // originX/originY: where this image sits in a larger one rendered in parts.
        noise_add_at(p, width, height, stride, float(num(s, "amount", 10)),
                     s.value("gaussian").toBool(), s.value("monochromatic").toBool(),
                     uint32_t(num(s, "seed", 1)), int64_t(num(s, "originX")),
                     int64_t(num(s, "originY")));
    } else if (kind == "Grain") {
        adjust_grain(p, width, height, stride, num(s, "amount", 20), num(s, "size", 1),
                     num(s, "roughness", 50), uint32_t(num(s, "seed", 1)), num(s, "originX"),
                     num(s, "originY"), 1);
    } else if (kind == "Gradient Map") {
        auto dark = s.value("shadows").toObject(), light = s.value("highlights").toObject();
        if (s.value("reversed").toBool())
            std::swap(dark, light);
        uchar table[768];
        const char *keys[3] = {"red", "green", "blue"};
        for (int i = 0; i < 256; ++i)
            for (int c = 0; c < 3; ++c)
                table[i * 3 + c] =
                    byte(255 * (num(dark, keys[c]) +
                                (num(light, keys[c], 1) - num(dark, keys[c])) * i / 255.0));
        adjust_gradient_map(p, width, height, stride, table);
    } else if (kind == "Color Balance") {
        float ranges[3][3]{};
        const char *keys[3][3] = {
            {"shadowCyanRed", "shadowMagentaGreen", "shadowYellowBlue"},
            {"midCyanRed", "midMagentaGreen", "midYellowBlue"},
            {"highlightCyanRed", "highlightMagentaGreen", "highlightYellowBlue"}};
        for (int t = 0; t < 3; ++t)
            for (int c = 0; c < 3; ++c)
                ranges[t][c] = float(num(s, keys[t][c]) / 100);
        adjust_color_balance(p, width, height, stride, ranges[0], ranges[1], ranges[2],
                             s.value("preserveLuminosity").toBool(true));
    } else if (kind == "Gaussian Blur") {
        out = gaussianBlur(out, num(s, "radius", 2));
    } else if (kind == "Motion Blur") {
        out = motionBlur(out, num(s, "angle"), num(s, "distance", 10));
    } else if (kind == "Lens Correction") {
        auto original = out;
        out.detach();
        lens_distort(original.constBits(), out.bits(), width, height, stride,
                     num(s, "distortion") / 100 * .35);
    } else if (kind == "Vignette") {
        auto color = s.value("vignetteColor").toObject();
        bool empty = true;
        for (int y = 0; y < out.height() && empty; ++y)
            for (int x = 0; x < out.width(); ++x)
                if (out.constScanLine(y)[x * 4 + 3]) {
                    empty = false;
                    break;
                }
        adjust_colored_vignette(p, width, height, stride, 0, 0, double(width), double(height),
                                s.value("fillsClear").toBool(empty), num(s, "vignetteAmount", 35),
                                num(s, "vignetteMidpoint", 50), num(s, "vignetteRoundness", 100),
                                num(s, "vignetteFeather", 60), num(s, "vignetteHighlights", 25),
                                num(color, "red"), num(color, "green"), num(color, "blue"));
    } else if (kind == "Tonal Contrast") {
        auto blurred = gaussianBlur(out, num(s, "tonalRadius", 16));
        adjust_tonal_contrast(p, blurred.constBits(), width, height, stride,
                              size_t(blurred.bytesPerLine()), num(s, "tonalAmount", 50),
                              num(s, "tonalShadows", 40), num(s, "tonalMidtones", 60),
                              num(s, "tonalHighlights", 30));
    } else if (kind == "Bloom / Glow") {
        auto bloom = gaussianBlur(out, num(s, "bloomRadius", 24));
        double amount = num(s, "bloomAmount", 40) / 50;
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x) {
                auto q = out.scanLine(y) + x * 4;
                auto b = bloom.constScanLine(y) + x * 4;
                for (int c = 0; c < 3; ++c)
                    q[c] = uchar(std::min(int(q[3]), int(std::lround(q[c] + b[c] * amount))));
            }
    } else if (kind == "Camera Raw") {
        double t = num(s, "temperature") / 100, tint = num(s, "tint") / 100,
               scale = num(s, "previewScale", 1);
        require(scale > 0 && scale <= 1, "Invalid Camera Raw preview scale");
        adjust_camera_raw_calibration(p, width, height, stride, num(s, "shadowTint"),
                                      num(s, "redHue"), num(s, "redSaturation"), num(s, "greenHue"),
                                      num(s, "greenSaturation"), num(s, "blueHue"),
                                      num(s, "blueSaturation"), int(num(s, "processVersion", 6)));
        adjust_camera_raw(
            p, width, height, stride, num(s, "redGain", 1 + .35 * t + .15 * tint),
            num(s, "greenGain", 1 - .30 * tint), num(s, "blueGain", 1 - .35 * t + .15 * tint),
            num(s, "exposure"), num(s, "contrast"), num(s, "highlights"), num(s, "shadows"),
            num(s, "whites"), num(s, "blacks"), num(s, "vibrance"), num(s, "saturation"), 0);
        applyCameraRawColor(out, s);
        adjust_camera_raw_effects(
            p, width, height, stride, num(s, "texture"), num(s, "clarity"), num(s, "dehaze"),
            num(s, "glow"), int(num(s, "glowStyle")), num(s, "glowRange"), num(s, "glowSpread"),
            num(s, "glowWarmth"), num(s, "vignetteAmount"), num(s, "vignetteMidpoint", 50),
            num(s, "vignetteRoundness"), num(s, "vignetteFeather", 50),
            num(s, "vignetteHighlights"), int(num(s, "vignetteStyle")), scale);
        if (num(s, "grainAmount") > 0)
            adjust_grain(p, width, height, stride, num(s, "grainAmount"),
                         .5 + num(s, "grainSize", 25) / 100 * 19.5, num(s, "grainRoughness", 50),
                         uint32_t(num(s, "seed", 1)), 0, 0, 1 / scale);
        bool profile = s.value("enableLensProfile").toBool();
        adjust_camera_raw_optics(
            p, width, height, stride, s.value("removeChromaticAberration").toBool(), profile,
            num(s, "profileDistortion", 100), num(s, "profileVignetting", 100),
            .35 * (num(s, "opticsDistortion") + (profile ? num(s, "profileDistortion", 100) : 0)) /
                100,
            num(s, "purpleAmount"), num(s, "purpleHueLow", 270), num(s, "purpleHueHigh", 310),
            num(s, "greenAmount"), num(s, "greenHueLow", 60), num(s, "greenHueHigh", 120),
            num(s, "opticsVignetteAmount"), num(s, "opticsVignetteMidpoint", 50), scale);
        adjust_camera_raw_detail(
            p, width, height, stride, num(s, "sharpenAmount"), num(s, "sharpenRadius", 10),
            num(s, "sharpenDetail", 25), num(s, "sharpenMasking"), num(s, "noiseLuminance"),
            num(s, "noiseLuminanceDetail", 50), num(s, "noiseLuminanceContrast"),
            num(s, "noiseColor"), num(s, "noiseColorDetail", 50),
            num(s, "noiseColorSmoothness", 50), scale);
    } else if (kind == "Content-Aware Fill") {
        QImage mask =
            QImage::fromData(QByteArray::fromBase64(s.value("maskPNG").toString().toLatin1()))
                .convertToFormat(QImage::Format_Grayscale8);
        require(mask.size() == out.size(), "Select an area to fill");
        const auto result = content_fill(p, stride, mask.constBits(), size_t(mask.bytesPerLine()),
                                         out.width(), out.height());
        require(result == 1, result == 0 ? "No usable source patch outside the selection"
                                         : "Not enough memory for content-aware fill");
    } else {
        throw Error("Filter not migrated yet: " + kind);
    }
    return out;
}
QJsonObject adjustmentSettings(const QJsonObject &a) {
    const auto kind = a.value("kind").toString();
    QJsonObject s;
    if (kind == "Exposure")
        s = a.value("exposureSettings").toObject();
    else if (kind == "Gradient Map")
        s = a.value("gradientMapSettings").toObject();
    else if (kind == "Black & White")
        s = a.value("blackWhiteSettings").toObject();
    else if (kind == "Color Balance")
        s = a.value("colorBalanceSettings").toObject();
    else if (kind == "Grain")
        s = a.value("grainSettings").toObject();
    else if (kind == "Levels")
        s = a.value("levels").toObject();
    else if (kind == "Curves")
        s = a.value("curves").toObject();
    else if (kind == "Hue/Saturation")
        s = a.value("hsvSettings").isObject() ? a.value("hsvSettings").toObject() : a;
    else if (kind == "Gaussian Blur")
        s["radius"] = a.value("blurRadius");
    else if (kind == "Motion Blur")
        s = {{"angle", a.value("motionAngle")}, {"distance", a.value("motionDistance")}};
    else if (kind == "Add Noise")
        s = {{"amount", a.value("noiseAmount")},
             {"gaussian", a.value("noiseGaussian")},
             {"monochromatic", a.value("noiseMonochromatic")},
             {"seed", a.value("noiseSeed")}};
    else
        s = a;
    if (kind == "Hue/Saturation" && s.contains("adjustments")) {
        s["adjustments"] = keyedDictionary(s.value("adjustments"));
        s["bands"] = keyedDictionary(s.value("bands"));
    }
    return s;
}
QJsonObject makeAdjustment(const QString &kind, const QJsonObject &settings) {
    QJsonObject a{{"kind", kind}};
    QString field;
    if (kind == "Exposure")
        field = "exposureSettings";
    else if (kind == "Levels")
        field = "levels";
    else if (kind == "Curves")
        field = "curves";
    else if (kind == "Gradient Map")
        field = "gradientMapSettings";
    else if (kind == "Black & White")
        field = "blackWhiteSettings";
    else if (kind == "Color Balance")
        field = "colorBalanceSettings";
    else if (kind == "Grain")
        field = "grainSettings";
    else if (kind == "Hue/Saturation")
        field = "hsvSettings";
    auto s = settings;
    if (kind == "Levels" && !s.contains("ranges")) {
        auto master = s;
        master["black"] = s.value("inputBlack");
        master["white"] = s.value("inputWhite");
        QJsonObject identity{
            {"black", 0}, {"white", 255}, {"gamma", 1}, {"outputBlack", 0}, {"outputWhite", 255}};
        s = {{"channel", "RGB"}, {"ranges", QJsonArray{master, identity, identity, identity}}};
    }
    if (kind == "Curves" && !s.contains("channels")) {
        QJsonArray identity{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 255}, {"y", 255}}};
        s = {{"channel", "RGB"},
             {"channels", QJsonArray{s.value("points").toArray(), identity, identity, identity}}};
    }
    if (kind == "Hue/Saturation") {
        if (!s.contains("range"))
            s["range"] = "Master";
        if (!s.contains("adjustments"))
            s["adjustments"] = QJsonObject{
                {s.value("range").toString(), QJsonObject{{"hue", s.value("hue")},
                                                          {"saturation", s.value("saturation")},
                                                          {"lightness", s.value("lightness")}}}};
        for (auto key : {"adjustments", "bands"}) {
            auto obj = keyedDictionary(s.value(QLatin1String(key)));
            QJsonArray entries;
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                entries.append(it.key());
                entries.append(it.value());
            }
            s[QLatin1String(key)] = entries;
        }
    }
    if (!field.isEmpty())
        a[field] = s;
    else if (kind == "Gaussian Blur")
        a["blurRadius"] = s.value("radius");
    else if (kind == "Motion Blur") {
        a["motionAngle"] = s.value("angle");
        a["motionDistance"] = s.value("distance");
    } else if (kind == "Add Noise") {
        a["noiseAmount"] = s.value("amount");
        a["noiseGaussian"] = s.value("gaussian");
        a["noiseMonochromatic"] = s.value("monochromatic");
        a["noiseSeed"] = s.value("seed").toInt();
    }
    return a;
}
QImage applyAdjustment(const QImage &source, const QJsonObject &a) {
    return applyFilter(source, a.value("kind").toString(), adjustmentSettings(a));
}
QImage limitToSelection(const QImage &original, const QImage &filtered, const QImage &coverage) {
    if (coverage.isNull())
        return filtered;
    require(original.size() == filtered.size() && original.size() == coverage.size(),
            "Selection dimensions differ");
    QImage out = original;
    out.detach();
    for (int y = 0; y < out.height(); ++y) {
        auto p = out.scanLine(y);
        auto q = filtered.constScanLine(y);
        auto mask = coverage.constScanLine(y);
        for (int x = 0; x < out.width(); ++x)
            for (int c = 0; c < 4; ++c)
                p[x * 4 + c] =
                    byte(p[x * 4 + c] * (1 - mask[x] / 255.0) + q[x * 4 + c] * mask[x] / 255.0);
    }
    return out;
}
} // namespace compositor
