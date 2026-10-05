// SPDX-License-Identifier: MIT
#include "adjustment_panels.h"
#include "language.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace compositor {
namespace {
constexpr double Margin = 8;
void drawTriangle(QPainter &p, double x, double top, QColor fill) {
    QPainterPath path;
    path.moveTo(x, top);
    path.lineTo(x - 6, top + 11);
    path.lineTo(x + 6, top + 11);
    path.closeSubpath();
    p.setPen(QPen(QColor(150, 150, 150), 1));
    p.setBrush(fill);
    p.drawPath(path);
}
} // namespace

LevelsGraph::LevelsGraph(QWidget *parent) : QWidget(parent) {
    setMinimumSize(300, 214);
    setToolTip("Drag the triangles to set the input and output levels.");
    setCursor(Qt::PointingHandCursor);
}
QSize LevelsGraph::sizeHint() const {
    return {380, 214};
}
QRectF LevelsGraph::histogramRect() const {
    return QRectF(Margin, 4, width() - 2 * Margin, 150);
}
QRectF LevelsGraph::inputStrip() const {
    return QRectF(Margin, 154, width() - 2 * Margin, 18);
}
QRectF LevelsGraph::outputStrip() const {
    return QRectF(Margin, 176, width() - 2 * Margin, 36);
}
double LevelsGraph::xOf(double value) const {
    return Margin + value / 255 * (width() - 2 * Margin);
}
double LevelsGraph::gammaPosition() const {
    return black + (white - black) * std::pow(.5, gamma);
}
void LevelsGraph::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto area = histogramRect();
    p.fillRect(area, QColor(24, 25, 28));
    if (!histogramReady) {
        p.setPen(QColor(170, 170, 170));
        p.drawText(area.adjusted(8, 6, 0, 0), Qt::AlignLeft | Qt::AlignTop, uiText("Loading histogram…"));
    } else {
        const auto &bins = histogram[size_t(std::clamp(channel, 0, 3))];
        const double peak = histogramScale(bins);
        const QColor colors[4] = {QColor(190, 190, 190), QColor(230, 80, 80), QColor(90, 200, 100),
                                  QColor(90, 140, 240)};
        if (peak > 0)
            for (int i = 0; i < 256; ++i) {
                const double h = area.height() * std::clamp(bins[i] / peak, 0.0, 1.0);
                p.fillRect(QRectF(area.left() + i * area.width() / 256, area.bottom() - h,
                                  area.width() / 256 + .5, h),
                           colors[std::clamp(channel, 0, 3)]);
            }
    }
    const auto input = inputStrip();
    drawTriangle(p, xOf(black), input.top() + 2, Qt::black);
    drawTriangle(p, xOf(gammaPosition()), input.top() + 2, QColor(128, 128, 128));
    drawTriangle(p, xOf(white), input.top() + 2, Qt::white);
    const auto output = outputStrip();
    QLinearGradient ramp(output.topLeft(), output.topRight());
    ramp.setColorAt(0, Qt::black);
    ramp.setColorAt(1, Qt::white);
    p.fillRect(QRectF(output.left(), output.top(), output.width(), 12), ramp);
    drawTriangle(p, xOf(outputBlack), output.top() + 16, Qt::black);
    drawTriangle(p, xOf(outputWhite), output.top() + 16, Qt::white);
}
void LevelsGraph::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    const double x = e->position().x();
    const bool output = e->position().y() >= outputStrip().top();
    QVector<QPair<QString, double>> handles =
        output ? QVector<QPair<QString, double>>{{"outputBlack", outputBlack},
                                                 {"outputWhite", outputWhite}}
               : QVector<QPair<QString, double>>{{"inputBlack", black},
                                                 {"gamma", gammaPosition()},
                                                 {"inputWhite", white}};
    double best = 1e9;
    for (const auto &handle : handles) {
        const double distance = std::abs(xOf(handle.second) - x);
        // Coincident handles: the one the pointer is past moves, so they can be pulled apart.
        if (distance < best - .5 || (std::abs(distance - best) <= .5 && x > xOf(handle.second))) {
            best = distance;
            dragging_ = handle.first;
        }
    }
    dragTo(x);
}
void LevelsGraph::mouseMoveEvent(QMouseEvent *e) {
    if (!dragging_.isEmpty())
        dragTo(e->position().x());
}
void LevelsGraph::mouseReleaseEvent(QMouseEvent *) {
    dragging_.clear();
}
void LevelsGraph::dragTo(double x) {
    if (dragging_.isEmpty())
        return;
    double value = std::clamp((x - Margin) / std::max(1.0, width() - 2 * Margin) * 255, 0.0, 255.0);
    if (dragging_ == "inputBlack")
        value = std::min(white - 1, std::round(value));
    else if (dragging_ == "inputWhite")
        value = std::max(black + 1, std::round(value));
    else if (dragging_ == "gamma") {
        const double fraction = std::clamp((value - black) / std::max(1.0, white - black), .001, .999);
        value = std::clamp(std::log(fraction) / std::log(.5), .1, 9.99);
    } else
        value = std::round(value);
    if (changed)
        changed(dragging_, value);
    update();
}

HueSpectrum::HueSpectrum(QWidget *parent) : QWidget(parent) {
    setMinimumSize(300, 52);
    setToolTip("Drag the handles to set the range of hues this adjustment changes.");
    band = defaultHueBand("Reds");
}
QSize HueSpectrum::sizeHint() const {
    return {380, 52};
}
double HueSpectrum::xOf(double degrees) const {
    return Margin + std::fmod(std::fmod(degrees, 360) + 360, 360) / 360 * (width() - 2 * Margin);
}
double HueSpectrum::degreesAt(double x) const {
    return std::clamp((x - Margin) / std::max(1.0, width() - 2 * Margin), 0.0, 1.0) * 360;
}
void HueSpectrum::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const double w = width() - 2 * Margin;
    for (int slice = 0; slice < 360; ++slice) {
        const QRectF top(Margin + slice * w / 360, 0, w / 360 + .6, 16);
        p.fillRect(top, QColor::fromHsvF(slice / 360.0, 1, 1));
        const QColor shifted = after.width() == 360 ? after.pixelColor(slice, 0)
                                                    : QColor::fromHsvF(slice / 360.0, 1, 1);
        p.fillRect(top.translated(0, 36), shifted);
    }
    const auto handles = hueHandles(band);
    // The band itself: shaded where full, lighter where it fades.
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < 4; ++i) {
        const double x = xOf(handles[size_t(i)]);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(235, 235, 235));
        if (i == 1 || i == 2)
            p.drawRect(QRectF(x - 1.5, 17, 3, 18));
        else
            p.drawRect(QRectF(x - 4, 22, 8, 8));
    }
}
void HueSpectrum::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    const auto handles = hueHandles(band);
    double best = 1e9;
    for (int i = 0; i < 4; ++i) {
        const double distance = std::abs(xOf(handles[size_t(i)]) - e->position().x());
        if (distance < best) {
            best = distance;
            dragging_ = i;
        }
    }
    mouseMoveEvent(e);
}
void HueSpectrum::mouseMoveEvent(QMouseEvent *e) {
    if (dragging_ < 0)
        return;
    band = moveHueHandle(band, dragging_, degreesAt(e->position().x()));
    if (changed)
        changed(band);
    update();
}
void HueSpectrum::mouseReleaseEvent(QMouseEvent *) {
    dragging_ = -1;
}
} // namespace compositor
