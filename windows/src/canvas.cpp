// SPDX-License-Identifier: MIT
#include "canvas.h"
#include "editable_layers.h"
#include "filters.h"
#include "render.h"
#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTextDocument>
#include <QUrl>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <vector>
extern "C" {
#include "HealPixels.h"
#include "WandPixels.h"
}

namespace compositor {
Canvas::Canvas(Document *document, QWidget *parent) : QWidget(parent), document_(document) {
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 200);
}
QRectF Canvas::canvasRect() const {
    const auto s = document_->size();
    const auto extent = QSizeF(s) * zoom;
    return QRectF(QPointF((width() - extent.width()) / 2, (height() - extent.height()) / 2) + pan_,
                  extent);
}
QPointF Canvas::toDocument(QPointF point) const {
    return (point - canvasRect().topLeft()) / zoom;
}
void Canvas::rebuildPreview() {
    auto s = document_->size();
    if (s.isEmpty())
        return;
    auto out = s;
    out.scale(1600, 1600, Qt::KeepAspectRatio);
    if (out.width() > s.width() && out.height() > s.height())
        out = s;
    preview_ = renderDocument(*document_, out);
}
void Canvas::refresh() {
    try {
        rebuildPreview();
    } catch (const std::exception &e) {
        emit error(QString::fromUtf8(e.what()));
    }
    update();
}
void Canvas::fit() {
    auto s = document_->size();
    if (s.isEmpty())
        return;
    zoom = std::min((width() - 64.0) / s.width(), (height() - 64.0) / s.height());
    zoom = std::max(0.01, zoom);
    pan_ = {};
    refresh();
}
void Canvas::setTool(Tool value) {
    if (dragging_ && value != tool) {
        dragging_ = false;
        warp_.reset();
        original_ = {};
        coverage_ = {};
        blurred_ = {};
        emit editCanceled();
    }
    tool = value;
    setCursor(tool == Tool::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
    update();
}
void Canvas::clearSelection() {
    selection = {};
    emit selectionChanged();
    update();
}
void Canvas::selectAll() {
    selection = QImage(document_->size(), QImage::Format_Grayscale8);
    if (selection.isNull()) {
        emit error("Not enough memory for selection");
        return;
    }
    selection.fill(255);
    emit selectionChanged();
    update();
}
void Canvas::invertSelection() {
    if (selection.isNull()) {
        selectAll();
        return;
    }
    selection.detach();
    for (int y = 0; y < selection.height(); ++y) {
        auto p = selection.scanLine(y);
        for (int x = 0; x < selection.width(); ++x)
            p[x] = 255 - p[x];
    }
    emit selectionChanged();
    update();
}
QRect Canvas::selectionBounds() const {
    if (selection.isNull())
        return {};
    int x0 = selection.width(), y0 = selection.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < selection.height(); ++y) {
        auto p = selection.constScanLine(y);
        for (int x = 0; x < selection.width(); ++x)
            if (p[x]) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    }
    return x1 >= x0 ? QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1) : QRect();
}
QImage Canvas::selectionForLayer(const Layer &layer) const {
    return layerSelection(*document_, layer, selection);
}
void Canvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(29, 31, 36));
    auto r = canvasRect();
    p.save();
    p.setClipRect(r);
    const int tile = 12;
    auto clipped = r.intersected(rect());
    for (int y = int(clipped.top()); y <= clipped.bottom(); y += tile)
        for (int x = int(clipped.left()); x <= clipped.right(); x += tile)
            p.fillRect(x, y, tile, tile,
                       ((x / tile + y / tile) & 1) ? QColor(88, 90, 95) : QColor(112, 114, 119));
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom < 1);
    if (!preview_.isNull())
        p.drawImage(r, preview_);
    const auto guides = document_->metadata.value("guides").toArray();
    p.setPen(QPen(QColor(55, 205, 220), 1));
    for (const auto &v : guides) {
        auto g = v.toObject();
        double position = g.value("position").toDouble() * zoom;
        if (g.value("axis") == "horizontal")
            p.drawLine(QPointF(r.left(), r.top() + position),
                       QPointF(r.right(), r.top() + position));
        else
            p.drawLine(QPointF(r.left() + position, r.top()),
                       QPointF(r.left() + position, r.bottom()));
    }
    p.restore();
    p.setPen(QColor(5, 6, 8));
    p.drawRect(r);
    if (!selection.isNull()) {
        if (selection.cacheKey() != selectionOutlineKey_) {
            selectionOutlineKey_ = selection.cacheKey();
            selectionOutline_ = QPainterPath();
            auto mask = selection;
            double scale = 1;
            if (std::max(mask.width(), mask.height()) > 2048) {
                mask = mask.scaled(2048, 2048, Qt::KeepAspectRatio, Qt::FastTransformation);
                scale = double(selection.width()) / mask.width();
            }
            auto selected = [&](int x, int y) {
                return mask.valid(x, y) && mask.constScanLine(y)[x] >= 128;
            };
            auto edge = [&](QPointF a, QPointF b) {
                selectionOutline_.moveTo(a * scale);
                selectionOutline_.lineTo(b * scale);
            };
            for (int y = 0; y < mask.height(); ++y)
                for (int x = 0; x < mask.width(); ++x)
                    if (selected(x, y)) {
                        if (!selected(x, y - 1))
                            edge({double(x), double(y)}, {double(x + 1), double(y)});
                        if (!selected(x, y + 1))
                            edge({double(x), double(y + 1)}, {double(x + 1), double(y + 1)});
                        if (!selected(x - 1, y))
                            edge({double(x), double(y)}, {double(x), double(y + 1)});
                        if (!selected(x + 1, y))
                            edge({double(x + 1), double(y)}, {double(x + 1), double(y + 1)});
                    }
        }
        if (!selectionOutline_.isEmpty()) {
            QTransform map;
            map.translate(r.left(), r.top());
            map.scale(zoom, zoom);
            auto outline = map.map(selectionOutline_);
            p.setPen(QPen(Qt::black, 1, Qt::DashLine));
            p.drawPath(outline);
            p.setPen(QPen(Qt::white, 1, Qt::DotLine));
            p.drawPath(outline);
        }
    }
    if (dragging_ && (tool == Tool::RectangleSelect || tool == Tool::EllipseSelect ||
                      tool == Tool::Crop || tool == Tool::Rectangle || tool == Tool::Ellipse ||
                      tool == Tool::Line || tool == Tool::Gradient)) {
        QRectF outline(r.topLeft() + start_ * zoom, r.topLeft() + last_ * zoom);
        p.setPen(QPen(Qt::white, 1, Qt::DashLine));
        if (tool == Tool::EllipseSelect || tool == Tool::Ellipse)
            p.drawEllipse(outline.normalized());
        else if (tool == Tool::Line || tool == Tool::Gradient)
            p.drawLine(outline.topLeft(), outline.bottomRight());
        else
            p.drawRect(outline.normalized());
    }
    if (dragging_ && tool == Tool::Lasso) {
        QTransform map;
        map.translate(r.x(), r.y());
        map.scale(zoom, zoom);
        p.setPen(QPen(Qt::white, 1, Qt::DashLine));
        p.drawPath(map.map(lasso_));
    }
}
void Canvas::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    if (preview_.isNull())
        fit();
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    setFocus();
    start_ = last_ = toDocument(e->position());
    try {
        if (tool == Tool::Pan) {
            dragging_ = true;
            start_ = last_ = e->position();
            return;
        }
        if (tool == Tool::Eyedropper) {
            auto s = preview_.size();
            int x = int(start_.x() * s.width() / document_->size().width()),
                y = int(start_.y() * s.height() / document_->size().height());
            if (preview_.valid(x, y))
                emit colorPicked(preview_.pixelColor(x, y));
            return;
        }
        if (tool == Tool::Clone && (e->modifiers() & Qt::AltModifier)) {
            cloneSource_ = start_;
            cloneReady_ = true;
            return;
        }
        if (tool == Tool::RectangleSelect || tool == Tool::EllipseSelect || tool == Tool::Lasso ||
            tool == Tool::Crop) {
            dragging_ = true;
            priorSelection_ = selection;
            selectionModifiers_ = e->modifiers();
            lasso_ = QPainterPath(start_);
            return;
        }
        if (tool == Tool::Wand) {
            auto composite = renderDocument(*document_);
            int x = int(start_.x()), y = int(start_.y());
            if (!composite.valid(x, y))
                return;
            QImage next(composite.size(), QImage::Format_Grayscale8);
            require(!next.isNull(), "Not enough memory for selection");
            std::vector<uchar> packed(size_t(next.width()) * next.height());
            auto result =
                wand_mask(composite.constBits(), size_t(composite.width()),
                          size_t(composite.height()), size_t(composite.bytesPerLine()), size_t(x),
                          size_t(y), 0, float(wandTolerance), wandContiguous, packed.data());
            require(result >= 0, "Magic wand ran out of memory");
            for (int row = 0; row < next.height(); ++row)
                std::copy_n(packed.data() + size_t(row) * next.width(), next.width(),
                            next.scanLine(row));
            if (!selection.isNull() && (e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier)))
                for (int row = 0; row < next.height(); ++row)
                    for (int col = 0; col < next.width(); ++col)
                        next.scanLine(row)[col] =
                            (e->modifiers() & Qt::AltModifier)
                                ? uchar(selection.constScanLine(row)[col] *
                                        (1 - next.constScanLine(row)[col] / 255.0))
                                : std::max(selection.constScanLine(row)[col],
                                           next.constScanLine(row)[col]);
            selection = next;
            emit selectionChanged();
            update();
            return;
        }
        if (tool == Tool::Text) {
            bool ok = false;
            auto text = QInputDialog::getMultiLineText(this, "Text Layer", "Text", {}, &ok);
            if (!ok || text.isEmpty())
                return;
            emit editStarted();
            QJsonObject style{
                {"content", text},     {"fontName", "Segoe UI"},  {"fontSize", brushSize},
                {"red", color.redF()}, {"green", color.greenF()}, {"blue", color.blueF()},
                {"alignment", "Left"}, {"tracking", 0},           {"leading", 0}};
            auto image = renderText(style);
            auto id = document_->addImage(text.left(30), image);
            auto l = document_->find(id);
            l->move(start_);
            l->metadata["text"] = style;
            emit editFinished("Text Layer");
            refresh();
            return;
        }
        if (tool == Tool::Rectangle || tool == Tool::Ellipse || tool == Tool::Line) {
            emit editStarted();
            dragging_ = true;
            return;
        }
        auto layer = document_->active();
        require(layer, "Select a layer first");
        require(!layer->group() || paintMask, "Select an image layer or a folder mask");
        if (tool == Tool::Move) {
            emit editStarted();
            dragging_ = true;
            return;
        }
        require(!(paintMask ? layer->mask : layer->image).isNull(), "Select a pixel layer or mask");
        require(!paintMask || tool == Tool::Brush || tool == Tool::Erase || tool == Tool::Blur,
                "Mask painting supports Brush, Eraser, and Blur");
        if (tool == Tool::Clone)
            require(cloneReady_, "Alt-click the canvas to set a clone source");
        require(!paintMask || !layer->mask.isNull(), "Add a mask before painting it");
        emit editStarted();
        dragging_ = true;
        original_ = paintMask ? layer->mask : layer->image;
        if (tool == Tool::Smudge || tool == Tool::Liquify) {
            warp_ = std::make_unique<WarpBrush>(
                original_, layer->placement(original_.size()), document_->size(),
                tool == Tool::Smudge ? WarpMode::Smudge : WarpMode::Liquify, brushSize, hardness,
                brushOpacity);
            warp_->append(start_);
            warpChanged_ = false;
            return;
        }
        if (tool == Tool::Blur) {
            auto placement = layer->placement(original_.size());
            if (paintMask && layer->metadata.value("maskPlacement").isObject()) {
                Layer maskLayer = *layer;
                maskLayer.metadata["transform"] = layer->metadata.value("maskPlacement");
                placement = maskLayer.placement(original_.size());
            }
            double scale = std::max(.000001, std::sqrt(std::abs(placement.determinant())));
            blurred_ = gaussianBlur(
                original_,
                std::min({std::clamp(blurRadius, .5, 50.0) / scale,
                          std::max(original_.width(), original_.height()) / 2.0, 250.0}),
                paintMask);
            if (paintMask)
                blurred_ = blurred_.convertToFormat(QImage::Format_Grayscale8);
        }
        coverage_ = QImage(original_.size(), QImage::Format_Grayscale8);
        require(!coverage_.isNull(), "Not enough memory for stroke");
        coverage_.fill(0);
        cloneOffset_ = cloneSource_ - start_;
        if (tool != Tool::Gradient)
            dab(start_);
        refresh();
    } catch (const std::exception &ex) {
        dragging_ = false;
        warp_.reset();
        original_ = {};
        coverage_ = {};
        blurred_ = {};
        emit error(QString::fromUtf8(ex.what()));
    }
}
void Canvas::dab(QPointF point) {
    auto layer = document_->active();
    if (!layer)
        return;
    QImage &target = paintMask ? layer->mask : layer->image;
    auto placement = layer->placement(target.size());
    if (paintMask && layer->metadata.value("maskPlacement").isObject()) {
        Layer maskLayer = *layer;
        maskLayer.metadata["transform"] = layer->metadata.value("maskPlacement");
        placement = maskLayer.placement(target.size());
    }
    const auto inverse = placement.inverted();
    const auto pixel = inverse.map(point);
    const double sx = std::hypot(placement.m11(), placement.m12()),
                 sy = std::hypot(placement.m21(), placement.m22());
    double rx = brushSize / (2 * std::max(0.001, sx)), ry = brushSize / (2 * std::max(0.001, sy));
    int x0 = std::max(0, int(std::floor(pixel.x() - rx))),
        x1 = std::min(target.width() - 1, int(std::ceil(pixel.x() + rx)));
    int y0 = std::max(0, int(std::floor(pixel.y() - ry))),
        y1 = std::min(target.height() - 1, int(std::ceil(pixel.y() + ry)));
    target.detach();
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            double radius = std::hypot((x + 0.5 - pixel.x()) / rx, (y + 0.5 - pixel.y()) / ry);
            if (radius > 1)
                continue;
            double amount = radius <= hardness ? 1 : (1 - radius) / std::max(0.001, 1 - hardness);
            auto docPoint = placement.map(QPointF(x + 0.5, y + 0.5));
            if (!selection.isNull()) {
                int px = int(std::floor(docPoint.x())), py = int(std::floor(docPoint.y()));
                if (!selection.valid(px, py))
                    continue;
                amount *= selection.constScanLine(py)[px] / 255.0;
            }
            auto mask = coverage_.scanLine(y) + x;
            *mask = std::max(*mask, uchar(std::clamp(std::lround(amount * 255), 0L, 255L)));
            amount = *mask / 255.0 * brushOpacity;
            if (tool == Tool::Heal)
                continue;
            if (paintMask) {
                auto v = tool == Tool::Blur    ? blurred_.constScanLine(y)[x]
                         : tool == Tool::Erase ? 0
                                               : qGray(color.rgb());
                target.scanLine(y)[x] =
                    uchar(std::lround(original_.constScanLine(y)[x] * (1 - amount) + v * amount));
                continue;
            }
            auto p = target.scanLine(y) + x * 4;
            auto base = original_.constScanLine(y) + x * 4;
            if (tool == Tool::Blur) {
                auto source = blurred_.constScanLine(y) + x * 4;
                for (int c = 0; c < 4; ++c)
                    p[c] = uchar(std::lround(base[c] * (1 - amount) + source[c] * amount));
            } else if (tool == Tool::Erase) {
                for (int c = 0; c < 4; ++c)
                    p[c] = uchar(std::lround(base[c] * (1 - amount)));
            } else if (tool == Tool::Clone) {
                auto q = inverse.map(docPoint + cloneOffset_);
                int cx = int(q.x()), cy = int(q.y());
                if (!original_.valid(cx, cy))
                    continue;
                auto source = original_.constScanLine(cy) + cx * 4;
                for (int c = 0; c < 4; ++c)
                    p[c] = uchar(std::lround(base[c] * (1 - amount) + source[c] * amount));
            } else {
                double alpha = amount * color.alphaF();
                for (int c = 0; c < 3; ++c) {
                    int v = c == 0 ? color.red() : c == 1 ? color.green() : color.blue();
                    p[c] = uchar(std::lround(base[c] * (1 - alpha) + v * alpha));
                }
                p[3] = uchar(std::lround(base[3] * (1 - alpha) + 255 * alpha));
            }
        }
    if (!paintMask && tool != Tool::Heal) {
        layer->metadata.remove("text");
        layer->metadata.remove("shape");
    }
    emit edited();
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    if (!dragging_)
        return;
    try {
        if (tool == Tool::Pan) {
            pan_ += e->position() - last_;
            last_ = e->position();
            update();
            return;
        }
        auto point = toDocument(e->position());
        if (warp_) {
            if (warp_->append(point)) {
                warpChanged_ = true;
                auto layer = document_->active();
                layer->image = warp_->result(original_, layer->placement(original_.size()),
                                             selectionForLayer(*layer));
                layer->metadata.remove("text");
                layer->metadata.remove("shape");
                emit edited();
                refresh();
            }
        } else if (tool == Tool::Move) {
            auto layer = document_->active();
            if (layer) {
                layer->move(point - last_);
                emit edited();
                refresh();
            }
        } else if (tool == Tool::Lasso) {
            lasso_.lineTo(point);
            update();
        } else if (tool == Tool::Brush || tool == Tool::Erase || tool == Tool::Clone ||
                   tool == Tool::Heal || tool == Tool::Blur) {
            auto delta = point - last_;
            int steps = std::max(1, int(std::ceil(std::hypot(delta.x(), delta.y()) /
                                                  std::max(1.0, brushSize * 0.1))));
            steps = std::min(steps, 1000);
            for (int i = 1; i <= steps; ++i)
                dab(last_ + delta * (double(i) / steps));
            refresh();
        } else
            update();
        last_ = point;
    } catch (const std::exception &ex) {
        dragging_ = false;
        warp_.reset();
        original_ = {};
        coverage_ = {};
        blurred_ = {};
        emit error(QString::fromUtf8(ex.what()));
    }
}
void Canvas::finishSelection() {
    QImage mask(document_->size(), QImage::Format_Grayscale8);
    require(!mask.isNull(), "Not enough memory for selection");
    mask.fill(0);
    QPainter p(&mask);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    auto r = QRectF(start_, last_).normalized();
    if (tool == Tool::EllipseSelect)
        p.drawEllipse(r);
    else if (tool == Tool::Lasso) {
        lasso_.closeSubpath();
        p.drawPath(lasso_);
    } else
        p.drawRect(r);
    p.end();
    if (!priorSelection_.isNull() &&
        (selectionModifiers_ & (Qt::ShiftModifier | Qt::AltModifier))) {
        for (int y = 0; y < mask.height(); ++y) {
            auto n = mask.scanLine(y);
            auto old = priorSelection_.constScanLine(y);
            for (int x = 0; x < mask.width(); ++x)
                n[x] = (selectionModifiers_ & Qt::AltModifier) ? uchar(old[x] * (1 - n[x] / 255.0))
                                                               : std::max(old[x], n[x]);
        }
    }
    selection = mask;
    emit selectionChanged();
}
void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (!dragging_ || e->button() != Qt::LeftButton)
        return;
    dragging_ = false;
    try {
        if (tool == Tool::Pan)
            return;
        last_ = toDocument(e->position());
        if (tool == Tool::RectangleSelect || tool == Tool::EllipseSelect || tool == Tool::Lasso ||
            tool == Tool::Crop) {
            finishSelection();
            update();
            return;
        }
        if (tool == Tool::Rectangle || tool == Tool::Ellipse || tool == Tool::Line) {
            auto r = QRectF(start_, last_).normalized();
            if (tool != Tool::Line && (r.width() < 1 || r.height() < 1))
                return;
            if (tool == Tool::Line) {
                auto bounds =
                    r.adjusted(-brushSize / 2, -brushSize / 2, brushSize / 2, brushSize / 2);
                QJsonObject style{
                    {"kind", "Line"},
                    {"red", color.redF()},
                    {"green", color.greenF()},
                    {"blue", color.blueF()},
                    {"cornerRadius", 0},
                    {"lineWidth", brushSize},
                    {"start", QJsonArray{(start_.x() - bounds.x()) / bounds.width(),
                                         (start_.y() - bounds.y()) / bounds.height()}},
                    {"end", QJsonArray{(last_.x() - bounds.x()) / bounds.width(),
                                       (last_.y() - bounds.y()) / bounds.height()}}};
                auto image = renderShape(style, bounds.size().toSize());
                auto id = document_->addImage("Line", image);
                document_->find(id)->setBounds(bounds);
                document_->find(id)->metadata["shape"] = style;
                emit editFinished("Line Layer");
                refresh();
                return;
            }
            QImage image(QSize(int(std::ceil(r.width())), int(std::ceil(r.height()))),
                         QImage::Format_RGBA8888_Premultiplied);
            require(!image.isNull(), "Not enough memory for shape");
            image.fill(Qt::transparent);
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            if (tool == Tool::Ellipse)
                p.drawEllipse(QRectF(QPointF(), image.size()));
            else
                p.drawRect(QRectF(QPointF(), image.size()));
            p.end();
            auto id = document_->addImage(tool == Tool::Ellipse ? "Ellipse" : "Rectangle", image);
            auto layer = document_->find(id);
            layer->setBounds(r);
            layer->metadata["shape"] =
                QJsonObject{{"kind", tool == Tool::Ellipse ? "Ellipse" : "Rectangle"},
                            {"red", color.redF()},
                            {"green", color.greenF()},
                            {"blue", color.blueF()},
                            {"cornerRadius", 0}};
            emit editFinished("Shape Layer");
            refresh();
            return;
        }
        auto layer = document_->active();
        if (!layer)
            return;
        if (warp_) {
            if (warp_->append(last_))
                warpChanged_ = true;
            if (warpChanged_) {
                layer->image = warp_->result(original_, layer->placement(original_.size()),
                                             selectionForLayer(*layer));
                layer->metadata.remove("text");
                layer->metadata.remove("shape");
            }
            warp_.reset();
            if (warpChanged_)
                emit editFinished(tool == Tool::Smudge ? "Smudge" : "Liquify");
            else
                emit editCanceled();
            original_ = {};
            refresh();
            return;
        }
        if (tool == Tool::Gradient) {
            auto from = layer->placement(layer->image.size()).inverted().map(start_),
                 to = layer->placement(layer->image.size()).inverted().map(last_);
            QImage painted = original_;
            painted.detach();
            QPainter p(&painted);
            QLinearGradient gradient(from, to);
            gradient.setColorAt(0, color);
            gradient.setColorAt(1, Qt::transparent);
            p.fillRect(painted.rect(), gradient);
            p.end();
            layer->image = limitToSelection(original_, painted, selectionForLayer(*layer));
            layer->metadata.remove("text");
            layer->metadata.remove("shape");
        }
        if (tool == Tool::Heal) {
            require(!paintMask, "Spot Healing currently targets image pixels");
            auto image = original_;
            image.detach();
            std::vector<uchar> packed(size_t(coverage_.width()) * coverage_.height());
            for (int y = 0; y < coverage_.height(); ++y)
                std::copy_n(coverage_.constScanLine(y), coverage_.width(),
                            packed.data() + size_t(y) * coverage_.width());
            require(spot_heal(image.bits(), packed.data(), size_t(image.width()),
                              size_t(image.height()), size_t(image.bytesPerLine()),
                              float(brushOpacity), 0, 1) == 0,
                    "Spot healing ran out of memory");
            layer->image = image;
            layer->metadata.remove("text");
            layer->metadata.remove("shape");
        }
        emit editFinished(tool == Tool::Move       ? "Move Layer"
                          : tool == Tool::Gradient ? "Gradient"
                          : tool == Tool::Heal     ? "Spot Healing"
                          : tool == Tool::Blur     ? "Blur Stroke"
                          : paintMask              ? "Paint Mask"
                                                   : "Brush Stroke");
        refresh();
    } catch (const std::exception &ex) {
        emit error(QString::fromUtf8(ex.what()));
    }
    original_ = {};
    coverage_ = {};
    blurred_ = {};
    warp_.reset();
}
void Canvas::wheelEvent(QWheelEvent *e) {
    auto point = toDocument(e->position());
    zoom = std::clamp(zoom * std::pow(1.15, e->angleDelta().y() / 120.0), 0.01, 64.0);
    auto now = canvasRect().topLeft() + point * zoom;
    pan_ += e->position() - now;
    update();
}
void Canvas::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void Canvas::dropEvent(QDropEvent *e) {
    QStringList paths;
    for (const auto &url : e->mimeData()->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    if (!paths.isEmpty()) {
        emit filesDropped(paths);
        e->acceptProposedAction();
    }
}
} // namespace compositor
