// SPDX-License-Identifier: MIT
#include "image_scope.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <numbers>
extern "C" {
#include "LevelsPixels.h"
}
namespace compositor {
ImageScope::ImageScope(QWidget *parent) : QWidget(parent) {
    setObjectName("imageScope");
    setMinimumSize(160, 160);
}
void ImageScope::setImage(const QImage &source) {
    auto image = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    std::array<double, 1024> bins{};
    if (!image.isNull())
        levels_histogram(image.constBits(), nullptr, size_t(image.width()) * image.height(),
                         bins.data());
    for (int c = 0; c < 3; ++c)
        std::copy_n(bins.begin() + (c + 1) * 256, 256, histograms[c].begin());
    vectors.fill(0);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            auto p = image.constScanLine(y) + x * 4;
            if (p[3] == 0)
                continue;
            auto color = QColor::fromRgbF(std::min(1.0, double(p[0]) / p[3]),
                                          std::min(1.0, double(p[1]) / p[3]),
                                          std::min(1.0, double(p[2]) / p[3]));
            double hue = color.hsvHueF(), saturation = color.hsvSaturationF();
            if (hue < 0 || saturation < 1e-4)
                continue;
            double angle = hue * 2 * std::numbers::pi;
            int column = std::clamp(int((.5 + std::cos(angle) * saturation * .48) * 64), 0, 63),
                row = std::clamp(int((.5 + std::sin(angle) * saturation * .48) * 64), 0, 63);
            vectors[size_t(row) * 64 + column] += p[3] / 255.0;
        }
    update();
}
void ImageScope::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(25, 28, 33));
    painter.setRenderHint(QPainter::Antialiasing);
    auto plot = QRectF(rect()).adjusted(6, 6, -6, -6);
    painter.setPen(QColor(70, 75, 83));
    if (mode == 0) {
        for (int i = 0; i <= 4; ++i)
            painter.drawLine(QPointF(plot.x() + plot.width() * i / 4, plot.y()),
                             QPointF(plot.x() + plot.width() * i / 4, plot.bottom()));
        double peak = 1;
        for (auto &channel : histograms) {
            QList<double> interior;
            for (int i = 1; i < 255; ++i)
                if (channel[i] > 0)
                    interior.append(channel[i]);
            auto maximum = *std::max_element(channel.begin(), channel.end());
            if (!interior.isEmpty()) {
                std::sort(interior.begin(), interior.end());
                maximum = std::min(maximum, interior[int((interior.size() - 1) * .95)] * 4);
            }
            peak = std::max(peak, maximum);
        }
        const QColor colors[]{QColor(240, 80, 80), QColor(80, 220, 115), QColor(85, 140, 255)};
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        for (int c = 0; c < 3; ++c) {
            QPainterPath path;
            path.moveTo(plot.bottomLeft());
            for (int i = 0; i < 256; ++i)
                path.lineTo(plot.x() + i / 255.0 * plot.width(),
                            plot.bottom() - std::min(1.0, histograms[c][i] / peak) * plot.height());
            path.lineTo(plot.bottomRight());
            path.closeSubpath();
            auto fill = colors[c];
            fill.setAlpha(100);
            painter.setPen(QPen(colors[c], 1));
            painter.setBrush(fill);
            painter.drawPath(path);
        }
    } else {
        double side = std::min(plot.width(), plot.height());
        QRectF box(plot.center().x() - side / 2, plot.center().y() - side / 2, side, side);
        QImage heat(64, 64, QImage::Format_ARGB32_Premultiplied);
        heat.fill(Qt::transparent);
        auto peak = *std::max_element(vectors.begin(), vectors.end());
        if (peak > 0)
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                    if (auto count = vectors[size_t(y) * 64 + x]; count > 0)
                        heat.setPixelColor(
                            x, 63 - y,
                            QColor(100, 230, 180, int(std::log1p(count) / std::log1p(peak) * 255)));
        painter.drawImage(box, heat);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(box.adjusted(side * .02, side * .02, -side * .02, -side * .02));
        painter.drawLine(QPointF(box.center().x(), box.top()),
                         QPointF(box.center().x(), box.bottom()));
        painter.drawLine(QPointF(box.left(), box.center().y()),
                         QPointF(box.right(), box.center().y()));
    }
}
} // namespace compositor
