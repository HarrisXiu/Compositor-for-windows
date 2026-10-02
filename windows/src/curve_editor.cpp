// SPDX-License-Identifier: MIT
#include "curve_editor.h"
#include "filters.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
namespace compositor {
CurveEditor::CurveEditor(QWidget *parent) : QWidget(parent) {
    setMinimumSize(260, 260);
    setToolTip("Click to add a point; drag to move; right-click to remove.");
    points = {QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 255}, {"y", 255}}};
}
QPointF CurveEditor::toValue(QPointF p) const {
    return {std::clamp((p.x() - 12) / (width() - 24) * 255, 0.0, 255.0),
            std::clamp((height() - 12 - p.y()) / (height() - 24) * 255, 0.0, 255.0)};
}
QPointF CurveEditor::toWidget(QPointF p) const {
    return {12 + p.x() / 255 * (width() - 24), height() - 12 - p.y() / 255 * (height() - 24)};
}
void CurveEditor::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 32, 36));
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor(65, 68, 74));
    for (int i = 0; i <= 4; ++i) {
        double v = 255 * i / 4.0;
        p.drawLine(toWidget({v, 0}), toWidget({v, 255}));
        p.drawLine(toWidget({0, v}), toWidget({255, v}));
    }
    QImage ramp(256, 1, QImage::Format_RGBA8888_Premultiplied);
    for (int x = 0; x < 256; ++x)
        ramp.setPixelColor(x, 0, QColor(x, x, x));
    auto mapped = applyFilter(ramp, "Curves", {{"points", points}});
    QPainterPath path;
    path.moveTo(toWidget({0, double(mapped.pixelColor(0, 0).red())}));
    for (int x = 1; x < 256; ++x)
        path.lineTo(toWidget({double(x), double(mapped.pixelColor(x, 0).red())}));
    p.setPen(QPen(QColor(95, 184, 255), 2));
    p.drawPath(path);
    p.setBrush(Qt::white);
    for (const auto &v : points) {
        auto o = v.toObject();
        p.drawEllipse(toWidget({o.value("x").toDouble(), o.value("y").toDouble()}), 4, 4);
    }
}
void CurveEditor::mousePressEvent(QMouseEvent *e) {
    auto value = toValue(e->position());
    int nearest = -1;
    double distance = 12;
    for (int i = 0; i < points.size(); ++i) {
        auto o = points[i].toObject();
        auto d = toWidget({o.value("x").toDouble(), o.value("y").toDouble()}) - e->position();
        double n = std::hypot(d.x(), d.y());
        if (n < distance) {
            distance = n;
            nearest = i;
        }
    }
    if (e->button() == Qt::RightButton) {
        if (nearest > 0 && nearest + 1 < points.size()) {
            points.removeAt(nearest);
            if (changed)
                changed(points);
            update();
        }
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (nearest < 0 && points.size() < 32) {
        int i = 1;
        while (i + 1 < points.size() && points[i].toObject().value("x").toDouble() < value.x())
            ++i;
        double lo = points[i - 1].toObject().value("x").toDouble(),
               hi = points[i].toObject().value("x").toDouble();
        if (hi - lo > 2) {
            value.setX(std::clamp(value.x(), lo + 1, hi - 1));
            points.insert(i, QJsonObject{{"x", value.x()}, {"y", value.y()}});
            nearest = i;
        }
    }
    dragging_ = nearest;
    mouseMoveEvent(e);
}
void CurveEditor::mouseMoveEvent(QMouseEvent *e) {
    if (dragging_ < 0)
        return;
    auto v = toValue(e->position());
    if (dragging_ == 0)
        v.setX(0);
    else if (dragging_ + 1 == points.size())
        v.setX(255);
    else
        v.setX(std::clamp(v.x(), points[dragging_ - 1].toObject().value("x").toDouble() + .01,
                          points[dragging_ + 1].toObject().value("x").toDouble() - .01));
    points[dragging_] = QJsonObject{{"x", v.x()}, {"y", v.y()}};
    if (changed)
        changed(points);
    update();
}
void CurveEditor::mouseReleaseEvent(QMouseEvent *) {
    dragging_ = -1;
}
} // namespace compositor
