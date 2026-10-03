// SPDX-License-Identifier: MIT
#include "canvas_tools.h"
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace compositor {
class CropTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *) override {
        before_ = c.session_->cropFrame.normalized();
        c.session_->cropBeforeGesture = before_;
        handle_ = 0;
        const auto point = c.start_;
        if (!before_.isEmpty()) {
            const double radius = 7 / c.zoom;
            if (point.y() >= before_.top() - radius && point.y() <= before_.bottom() + radius) {
                if (std::abs(point.x() - before_.left()) <= radius)
                    handle_ |= 1;
                if (std::abs(point.x() - before_.right()) <= radius)
                    handle_ |= 2;
            }
            if (point.x() >= before_.left() - radius && point.x() <= before_.right() + radius) {
                if (std::abs(point.y() - before_.top()) <= radius)
                    handle_ |= 4;
                if (std::abs(point.y() - before_.bottom()) <= radius)
                    handle_ |= 8;
            }
            if (!handle_ && before_.contains(point))
                handle_ = 16;
        }
        c.dragging_ = true;
        c.update();
    }
    void move(QMouseEvent *event) override {
        auto point = c.toDocument(event->position());
        const bool snap = !(event->modifiers() & Qt::ControlModifier);
        if (handle_ == 16) {
            auto frame = before_.translated(point - c.start_);
            if (snap) {
                const double dx1 = snapped(frame.left(), false) - frame.left(),
                             dx2 = snapped(frame.right(), false) - frame.right(),
                             dy1 = snapped(frame.top(), true) - frame.top(),
                             dy2 = snapped(frame.bottom(), true) - frame.bottom();
                auto nearest = [](double a, double b) {
                    return !a ? b : !b ? a : std::abs(a) < std::abs(b) ? a : b;
                };
                frame.translate(nearest(dx1, dx2), nearest(dy1, dy2));
            }
            c.session_->cropFrame = frame;
        } else {
            if (snap) {
                point.setX(snapped(point.x(), false));
                point.setY(snapped(point.y(), true));
            }
            QPointF fixed = c.start_;
            if (handle_) {
                fixed = QPointF(handle_ & 1 ? before_.right() : before_.left(),
                                handle_ & 4 ? before_.bottom() : before_.top());
                if (!(handle_ & 3))
                    point.setX(before_.right());
                if (!(handle_ & 12))
                    point.setY(before_.bottom());
            }
            if (event->modifiers() & Qt::AltModifier)
                fixed = handle_ ? before_.center() : c.start_;
            const double ratio = c.session_->cropRatio > 0 ? c.session_->cropRatio
                                 : event->modifiers() & Qt::ShiftModifier
                                     ? (before_.isEmpty() ? 1 : before_.width() / before_.height())
                                     : 0;
            auto delta = point - fixed;
            if (ratio > 0 && std::abs(delta.x()) + std::abs(delta.y()) > 0) {
                if (std::abs(delta.x()) > std::abs(delta.y()) * ratio)
                    delta.setY(std::copysign(std::abs(delta.x()) / ratio, delta.y()));
                else
                    delta.setX(std::copysign(std::abs(delta.y()) * ratio, delta.x()));
            }
            const auto other = event->modifiers() & Qt::AltModifier ? fixed - delta : fixed;
            c.session_->cropFrame = QRectF(other, fixed + delta).normalized();
        }
        c.update();
    }
    void release(QMouseEvent *event) override {
        move(event);
        if (c.session_->cropFrame.width() < 1 || c.session_->cropFrame.height() < 1)
            c.session_->cropFrame = {};
        emit c.sessionChanged();
    }
    bool keyPress(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            c.applyCropFrame();
        else if (event->key() == Qt::Key_Escape)
            c.cancelCropFrame();
        else
            return false;
        event->accept();
        return true;
    }
    void paintOverlay(QPainter &painter) override {
        if (c.session_->cropFrame.isEmpty())
            return;
        const auto canvas = c.canvasRect();
        const QRectF frame(canvas.topLeft() + c.session_->cropFrame.topLeft() * c.zoom,
                           c.session_->cropFrame.size() * c.zoom);
        painter.save();
        QPainterPath shade;
        shade.addRect(canvas);
        shade.addRect(frame);
        painter.setClipRect(canvas);
        painter.fillPath(shade, QColor(0, 0, 0, 150));
        painter.setClipping(false);
        painter.setPen(QPen(Qt::white, 1));
        painter.drawRect(frame);
        painter.setPen(QPen(QColor(255, 255, 255, 130), 1));
        for (int i = 1; i <= 2; ++i) {
            painter.drawLine(QPointF(frame.left() + frame.width() * i / 3, frame.top()),
                             QPointF(frame.left() + frame.width() * i / 3, frame.bottom()));
            painter.drawLine(QPointF(frame.left(), frame.top() + frame.height() * i / 3),
                             QPointF(frame.right(), frame.top() + frame.height() * i / 3));
        }
        painter.setBrush(Qt::white);
        painter.setPen(QColor(40, 40, 40));
        for (const auto &point :
             {frame.topLeft(), frame.topRight(), frame.bottomLeft(), frame.bottomRight(),
              QPointF(frame.center().x(), frame.top()), QPointF(frame.center().x(), frame.bottom()),
              QPointF(frame.left(), frame.center().y()),
              QPointF(frame.right(), frame.center().y())})
            painter.drawRect(QRectF(point - QPointF(3, 3), QSizeF(6, 6)));
        painter.restore();
    }

  private:
    QRectF before_;
    int handle_ = 0;
    double snapped(double value, bool horizontal) const {
        QVector<double> candidates{
            0, double(horizontal ? c.document_->size().height() : c.document_->size().width())};
        for (const auto &entry : c.document_->metadata.value("guides").toArray()) {
            auto guide = entry.toObject();
            if ((guide.value("axis") == "horizontal") == horizontal)
                candidates << guide.value("position").toDouble();
        }
        for (const auto &layer : c.document_->layers) {
            if (layer.group() || !layer.visible())
                continue;
            const QSize source = layer.image.isNull() ? QSize(1, 1) : layer.image.size();
            const auto bounds = layer.placement(source).mapRect(QRectF(QPointF(), source));
            candidates << (horizontal ? bounds.top() : bounds.left())
                       << (horizontal ? bounds.bottom() : bounds.right());
        }
        double closest = value, distance = 6 / c.zoom;
        for (double candidate : candidates)
            if (std::abs(candidate - value) < distance) {
                distance = std::abs(candidate - value);
                closest = candidate;
            }
        return closest;
    }
};
std::unique_ptr<CanvasTool> makeCropTool(Canvas &canvas) {
    return std::make_unique<CropTool>(canvas);
}
} // namespace compositor
