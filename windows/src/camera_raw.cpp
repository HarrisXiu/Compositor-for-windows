// SPDX-License-Identifier: MIT
#include "camera_raw.h"
#include "document.h"
#include <QColorSpace>
#include <QJsonArray>
#include <QPainter>
#include <QPolygonF>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
extern "C" {
#include "AdjustPixels.h"
}
namespace compositor {
namespace {
double number(const QJsonObject &s, const QString &key, double initial = 0, double low = -100,
              double high = 100) {
    double v = s.value(key).toDouble(initial);
    require(std::isfinite(v), "Invalid Camera Raw setting");
    return std::clamp(v, low, high);
}
QJsonArray curve(const QJsonObject &s, const QString &key) {
    auto points = s.value(key).toArray();
    if (points.isEmpty())
        return {QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 255}, {"y", 255}}};
    return points;
}
double bend(double x, double lower, double low, double upper, double high) {
    if (x < lower && lower > 0)
        return lower * std::pow(x / lower, std::pow(2.0, -low / 100 * 1.66));
    if (x > upper && upper < 1)
        return 1 - (1 - upper) * std::pow((1 - x) / (1 - upper), std::pow(2.0, high / 100 * 1.66));
    return x;
}
} // namespace
QJsonObject cameraRawWhiteBalance(const QImage &source) {
    auto image = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    std::array<double, 3> average{};
    double count = 0;
    auto decode = [](double v) {
        return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
    };
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            auto p = image.constScanLine(y) + x * 4;
            if (p[3] == 0)
                continue;
            for (int c = 0; c < 3; ++c)
                average[c] += decode(std::min(1.0, double(p[c]) / p[3]));
            ++count;
        }
    if (count == 0)
        return {};
    for (auto &channel : average)
        channel /= count;
    double red = average[0], green = average[1], blue = average[2];
    if (red <= 1e-4 || green <= 1e-4 || blue <= 1e-4)
        return {};
    double a1 = .35 * red, b1 = .15 * red + .30 * green, c1 = green - red, a2 = -.35 * blue,
           b2 = .15 * blue + .30 * green, c2 = green - blue, determinant = a1 * b2 - a2 * b1;
    if (std::abs(determinant) <= 1e-8)
        return {};
    return {{"temperature", (c1 * b2 - c2 * b1) / determinant * 100},
            {"tint", (a1 * c2 - a2 * c1) / determinant * 100}};
}
void cameraRawPreviewOverlay(QImage &image, const QJsonObject &s) {
    image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (s.value("previewSharpenMask").toBool())
        adjust_camera_raw_sharpen_mask_overlay(
            image.bits(), size_t(image.width()), size_t(image.height()),
            size_t(image.bytesPerLine()), number(s, "sharpenRadius", 10, 0, 100),
            number(s, "sharpenDetail", 25, 0, 100), number(s, "sharpenMasking", 0, 0, 100),
            number(s, "previewScale", 1, .00001, 1));
    else if (s.value("previewShadowClipping").toBool() ||
             s.value("previewHighlightClipping").toBool())
        adjust_camera_raw_clip_overlay(image.bits(), size_t(image.width()), size_t(image.height()),
                                       size_t(image.bytesPerLine()),
                                       s.value("previewShadowClipping").toBool(),
                                       s.value("previewHighlightClipping").toBool());
}
void applyCameraRawColor(QImage &image, const QJsonObject &s) {
    std::array<std::array<float, 256>, 4> tables{};
    const QStringList channelKeys{"curveRGB", "curveRed", "curveGreen", "curveBlue"};
    const QJsonArray identity{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 255}, {"y", 255}}};
    double shadows = number(s, "curveShadows"), darks = number(s, "curveDarks"),
           lights = number(s, "curveLights"), highlights = number(s, "curveHighlights"),
           shadowSplit = number(s, "curveShadowSplit", 25, 5, 90),
           darkSplit = number(s, "curveDarkSplit", 50, shadowSplit + 2, 95),
           lightSplit = number(s, "curveLightSplit", 75, darkSplit + 2, 98);
    QJsonArray anchors;
    for (int i = 0; i <= 32; ++i) {
        double x = i / 32.0;
        double y = bend(bend(x, shadowSplit / 100, shadows, lightSplit / 100, highlights),
                        darkSplit / 100, darks, darkSplit / 100, lights);
        anchors.append(QJsonObject{{"x", x * 255}, {"y", y * 255}});
    }
    bool parametric = shadows != 0 || darks != 0 || lights != 0 || highlights != 0;
    bool active = parametric || number(s, "curveRefineSaturation") != 0;
    for (int c = 0; c < 4; ++c) {
        auto points = curve(s, channelKeys[c]);
        active = active || points != identity;
        for (int i = 0; i < 256; ++i) {
            double x = c == 0 && parametric ? curveValue(i, anchors) : i;
            tables[c][i] = float(curveValue(x, points) / 255);
        }
    }
    const QStringList families{"Reds",  "Oranges", "Yellows", "Greens",
                               "Aquas", "Blues",   "Purples", "Magentas"};
    const QStringList components{"Hue", "Saturation", "Luminance"};
    std::array<float, 24> mixer{};
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 8; ++i)
            mixer[c * 8 + i] = float(number(s, "mixer" + families[i] + components[c]) / 100);
    for (auto v : mixer)
        active = active || v != 0;
    auto points = s.value("pointColors").toArray();
    require(points.size() <= 8, "At most eight point colors are supported");
    std::array<float, 72> picked{};
    for (int i = 0; i < points.size(); ++i) {
        auto p = points[i].toObject();
        float *v = picked.data() + i * 9;
        v[0] = float(number(p, "hue", 0, 0, 360) / 360);
        v[1] = float(number(p, "saturation", 0, 0, 1));
        v[2] = float(number(p, "luminance", 0, 0, 1));
        v[3] = float(number(p, "hueShift") / 100);
        v[4] = float(number(p, "saturationShift") / 100);
        v[5] = float(number(p, "luminanceShift") / 100);
        v[6] = float(number(p, "hueRange", 30, 5, 180) / 360);
        v[7] = float(number(p, "saturationRange", .4, .05, 1));
        v[8] = float(number(p, "luminanceRange", .4, .05, 1));
        active = active || v[3] != 0 || v[4] != 0 || v[5] != 0;
    }
    std::array<float, 12> grade{};
    const QStringList wheels{"Shadows", "Midtones", "Highlights", "Global"};
    for (int i = 0; i < 4; ++i) {
        grade[i * 3] = float(number(s, "grade" + wheels[i] + "Hue", 0, 0, 360) / 360);
        grade[i * 3 + 1] = float(number(s, "grade" + wheels[i] + "Saturation", 0, 0, 100) / 100);
        grade[i * 3 + 2] = float(number(s, "grade" + wheels[i] + "Luminance") / 100);
        active = active || grade[i * 3 + 1] != 0 || grade[i * 3 + 2] != 0;
    }
    if (!active && number(s, "visualizePointColor", -1, -1, 7) < 0)
        return;
    adjust_camera_raw_curve_color(
        image.bits(), size_t(image.width()), size_t(image.height()), size_t(image.bytesPerLine()),
        tables[0].data(), tables[1].data(), tables[2].data(), tables[3].data(),
        number(s, "curveRefineSaturation") / 100, mixer.data(), int(points.size()), picked.data(),
        grade.data(), number(s, "gradeBlending", 50, 0, 100) / 100, number(s, "gradeBalance") / 100,
        int(number(s, "visualizePointColor", -1, -1, 7)));
}
QImage cameraRawGeometry(const QImage &image, const QJsonObject &s) {
    double vertical = number(s, "geometryVertical"), horizontal = number(s, "geometryHorizontal"),
           rotation = number(s, "geometryRotate", 0, -45, 45), aspect = number(s, "geometryAspect"),
           zoom = number(s, "geometryScale"), offsetX = number(s, "geometryOffsetX"),
           offsetY = number(s, "geometryOffsetY");
    QList<QLineF> guides;
    auto entries = s.value("geometryGuides").toArray();
    require(entries.size() <= 4, "At most four geometry guides are supported");
    for (auto v : entries) {
        auto g = v.toObject();
        QLineF line(number(g, "startX", 0, 0, 1), number(g, "startY", 0, 0, 1),
                    number(g, "endX", 0, 0, 1), number(g, "endY", 0, 0, 1));
        if (line.length() > .01)
            guides.append(line);
    }
    bool guided = s.value("geometryUpright").toBool() && !guides.isEmpty();
    if (!guided && vertical == 0 && horizontal == 0 && rotation == 0 && aspect == 0 && zoom == 0 &&
        offsetX == 0 && offsetY == 0)
        return image;
    if (guided) {
        auto angle = [](const QLineF &l) {
            return std::atan2(l.dy(), l.dx()) * 180 / std::numbers::pi;
        };
        double turn = -angle(guides.first());
        if (turn > 45)
            turn -= 90;
        else if (turn < -45)
            turn += 90;
        rotation += turn;
        if (guides.size() > 1) {
            double a = angle(guides[1]);
            if (std::abs(a) > 45)
                vertical += a > 0 ? 25 : -25;
            else
                horizontal += a > 0 ? 25 : -25;
        }
    }
    double w = image.width(), h = image.height(),
           strength = s.value("geometryProjection").toInt() == 1 ? .55 : 1,
           v = vertical / 100 * w * .18 * strength, hz = horizontal / 100 * h * .18 * strength,
           shiftX = offsetX / 100 * w * .15, shiftY = offsetY / 100 * h * .15;
    QPointF center(w / 2 + shiftX, h / 2 + shiftY);
    QPolygonF corners{QPointF(-v + shiftX, h + shiftY), QPointF(w + v + shiftX, h + shiftY),
                      QPointF(w + hz + shiftX, -shiftY), QPointF(-hz + shiftX, -shiftY)};
    double rad = rotation * std::numbers::pi / 180, a = 1 + aspect / 200, z = 1 + zoom / 100;
    for (auto &point : corners) {
        auto d = point - center;
        point = center + QPointF((d.x() * std::cos(rad) - d.y() * std::sin(rad)) * a * z,
                                 (d.x() * std::sin(rad) + d.y() * std::cos(rad)) / a * z);
        point.setY(h - point.y());
    }
    QPolygonF input{QPointF(0, 0), QPointF(w, 0), QPointF(w, h), QPointF(0, h)};
    QTransform transform;
    QImage out(image.size(), QImage::Format_RGBA8888_Premultiplied);
    require(!out.isNull(), "Not enough memory for geometry correction");
    out.setColorSpace(image.colorSpace());
    out.fill(Qt::transparent);
    if (z == 0)
        return out;
    require(QTransform::quadToQuad(input, corners, transform), "Geometry correction is degenerate");
    {
        QPainter painter(&out);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setTransform(transform);
        painter.drawImage(QPointF(), image);
    }
    if (s.value("geometryConstrainCrop").toBool()) {
        int left = out.width(), top = out.height(), right = 0, bottom = 0;
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x)
                if (out.constScanLine(y)[x * 4 + 3]) {
                    left = std::min(left, x);
                    top = std::min(top, y);
                    right = std::max(right, x + 1);
                    bottom = std::max(bottom, y + 1);
                }
        if (left < right && top < bottom && (right - left < w || bottom - top < h)) {
            auto cropped = out.copy(left, top, right - left, bottom - top)
                               .scaled(out.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            out.fill(Qt::transparent);
            QPainter painter(&out);
            painter.drawImage(
                QPoint((out.width() - cropped.width()) / 2, (out.height() - cropped.height()) / 2),
                cropped);
        }
    }
    return out;
}
} // namespace compositor
