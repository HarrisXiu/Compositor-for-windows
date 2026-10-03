// SPDX-License-Identifier: MIT
#include "canvas.h"
#include "canvas_tools.h"
#include "editable_layers.h"
#include "filters.h"
#include "render.h"
#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFocusEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
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
Canvas::Canvas(Document *document, QWidget *parent) : Canvas(document, nullptr, parent) {}
Canvas::Canvas(Document *document, EditorSession *session, QWidget *parent)
    : QWidget(parent), document_(document), session_(session ? session : &fallbackSession_) {
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 200);
}
Canvas::~Canvas() = default;
CanvasTool &Canvas::controller() {
    auto kind = temporaryPan_ ? Tool::Pan : session_->tool;
    if (!controller_ || controllerKind_ != kind) {
        controller_ = makeCanvasTool(kind, *this);
        controllerKind_ = kind;
    }
    return *controller_;
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
    if (value != session_->tool) {
        cancelInteraction();
        session_->cropFrame = {};
        if (value == Tool::Crop && !session_->selection.isNull())
            session_->cropFrame = selectionBounds();
    }
    session_->tool = value;
    setCursor(session_->tool == Tool::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
    update();
    emit sessionChanged();
}
void Canvas::applyCropFrame() {
    const auto frame = session_->cropFrame.normalized();
    if (frame.width() < 1 || frame.height() < 1)
        return;
    cancelInteraction();
    const QRect bounds(
        QPoint(int(std::floor(frame.left())), int(std::floor(frame.top()))),
        QPoint(int(std::ceil(frame.right())) - 1, int(std::ceil(frame.bottom())) - 1));
    session_->cropFrame = {};
    emit cropRequested(bounds);
    update();
}
void Canvas::cancelCropFrame() {
    cancelInteraction();
    session_->cropFrame = {};
    update();
}
void Canvas::mouseDoubleClickEvent(QMouseEvent *event) {
    if (session_->tool == Tool::Crop && event->button() == Qt::LeftButton) {
        applyCropFrame();
        event->accept();
    } else
        QWidget::mouseDoubleClickEvent(event);
}
void Canvas::cancelInteraction() {
    if (!dragging_)
        return;
    dragging_ = false;
    if (session_->tool == Tool::Crop && !temporaryPan_)
        session_->cropFrame = session_->cropBeforeGesture;
    warp_.reset();
    original_ = {};
    coverage_ = {};
    blurred_ = {};
    lasso_ = {};
    emit editCanceled();
    update();
}
void Canvas::replaceSelection(const QImage &mask, const QString &label) {
    require(mask.isNull() ||
                (mask.size() == document_->size() && mask.format() == QImage::Format_Grayscale8),
            "Selection must match the canvas size");
    if (mask == session_->selection)
        return;
    auto before = session_->selection;
    session_->selection = mask;
    emit selectionEdited(label, before, mask);
    emit selectionChanged();
    update();
}
void Canvas::clearSelection() {
    cancelInteraction();
    replaceSelection({}, "Deselect");
}
void Canvas::selectAll() {
    cancelInteraction();
    QImage next(document_->size(), QImage::Format_Grayscale8);
    if (next.isNull()) {
        emit error("Not enough memory for selection");
        return;
    }
    next.fill(255);
    replaceSelection(next, "Select All");
}
void Canvas::invertSelection() {
    cancelInteraction();
    if (session_->selection.isNull()) {
        selectAll();
        return;
    }
    auto next = session_->selection;
    next.detach();
    for (int y = 0; y < next.height(); ++y) {
        auto p = next.scanLine(y);
        for (int x = 0; x < next.width(); ++x)
            p[x] = 255 - p[x];
    }
    replaceSelection(next, "Inverse Selection");
}
void Canvas::featherSelection(double radius) {
    cancelInteraction();
    if (session_->selection.isNull())
        return;
    auto rgba = session_->selection.convertToFormat(QImage::Format_RGBA8888);
    replaceSelection(gaussianBlur(rgba, radius).convertToFormat(QImage::Format_Grayscale8),
                     "Feather Selection");
}
QRect Canvas::selectionBounds() const {
    if (session_->selection.isNull())
        return {};
    int x0 = session_->selection.width(), y0 = session_->selection.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < session_->selection.height(); ++y) {
        auto p = session_->selection.constScanLine(y);
        for (int x = 0; x < session_->selection.width(); ++x)
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
    return layerSelection(*document_, layer, session_->selection);
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
    if (!session_->selection.isNull()) {
        if (session_->selection.cacheKey() != selectionOutlineKey_) {
            selectionOutlineKey_ = session_->selection.cacheKey();
            selectionOutline_ = QPainterPath();
            auto mask = session_->selection;
            double scale = 1;
            if (std::max(mask.width(), mask.height()) > 2048) {
                mask = mask.scaled(2048, 2048, Qt::KeepAspectRatio, Qt::FastTransformation);
                scale = double(session_->selection.width()) / mask.width();
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
    controller().paintOverlay(p);
}
void Canvas::drawGesture(QPainter &p) {
    auto r = canvasRect();
    if (dragging_ && (session_->tool == Tool::RectangleSelect ||
                      session_->tool == Tool::EllipseSelect || session_->tool == Tool::Crop ||
                      session_->tool == Tool::Rectangle || session_->tool == Tool::Ellipse ||
                      session_->tool == Tool::Line || session_->tool == Tool::Gradient)) {
        QRectF outline(r.topLeft() + start_ * zoom, r.topLeft() + last_ * zoom);
        p.setPen(QPen(Qt::white, 1, Qt::DashLine));
        if (session_->tool == Tool::EllipseSelect || session_->tool == Tool::Ellipse)
            p.drawEllipse(outline.normalized());
        else if (session_->tool == Tool::Line || session_->tool == Tool::Gradient)
            p.drawLine(outline.topLeft(), outline.bottomRight());
        else
            p.drawRect(outline.normalized());
    }
    if (dragging_ && session_->tool == Tool::Lasso) {
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
void Canvas::dab(QPointF point) {
    auto layer = document_->active();
    if (!layer)
        return;
    QImage &target = paintMask() ? layer->mask : layer->image;
    auto placement = layer->placement(target.size());
    if (paintMask() && layer->metadata.value("maskPlacement").isObject()) {
        Layer maskLayer = *layer;
        maskLayer.metadata["transform"] = layer->metadata.value("maskPlacement");
        placement = maskLayer.placement(target.size());
    }
    const auto inverse = placement.inverted();
    const auto pixel = inverse.map(point);
    const double sx = std::hypot(placement.m11(), placement.m12()),
                 sy = std::hypot(placement.m21(), placement.m22());
    double rx = session_->brushSize / (2 * std::max(0.001, sx)),
           ry = session_->brushSize / (2 * std::max(0.001, sy));
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
            double amount = radius <= session_->hardness
                                ? 1
                                : (1 - radius) / std::max(0.001, 1 - session_->hardness);
            auto docPoint = placement.map(QPointF(x + 0.5, y + 0.5));
            if (!session_->selection.isNull()) {
                int px = int(std::floor(docPoint.x())), py = int(std::floor(docPoint.y()));
                if (!session_->selection.valid(px, py))
                    continue;
                amount *= session_->selection.constScanLine(py)[px] / 255.0;
            }
            auto mask = coverage_.scanLine(y) + x;
            *mask = std::max(*mask, uchar(std::clamp(std::lround(amount * 255), 0L, 255L)));
            amount = *mask / 255.0 * session_->brushOpacity;
            if (session_->tool == Tool::Heal)
                continue;
            if (paintMask()) {
                auto v = session_->tool == Tool::Blur    ? blurred_.constScanLine(y)[x]
                         : session_->tool == Tool::Erase ? 0
                                                         : qGray(session_->foreground.rgb());
                target.scanLine(y)[x] =
                    uchar(std::lround(original_.constScanLine(y)[x] * (1 - amount) + v * amount));
                continue;
            }
            auto p = target.scanLine(y) + x * 4;
            auto base = original_.constScanLine(y) + x * 4;
            if (session_->tool == Tool::Blur) {
                auto source = blurred_.constScanLine(y) + x * 4;
                for (int c = 0; c < 4; ++c)
                    p[c] = uchar(std::lround(base[c] * (1 - amount) + source[c] * amount));
            } else if (session_->tool == Tool::Erase) {
                for (int c = 0; c < 4; ++c)
                    p[c] = uchar(std::lround(base[c] * (1 - amount)));
            } else if (session_->tool == Tool::Clone) {
                auto q = inverse.map(docPoint + cloneOffset_);
                int cx = int(q.x()), cy = int(q.y());
                if (!original_.valid(cx, cy))
                    continue;
                auto source = original_.constScanLine(cy) + cx * 4;
                for (int c = 0; c < 4; ++c)
                    p[c] = uchar(std::lround(base[c] * (1 - amount) + source[c] * amount));
            } else {
                double alpha = amount * session_->foreground.alphaF();
                for (int c = 0; c < 3; ++c) {
                    int v = c == 0   ? session_->foreground.red()
                            : c == 1 ? session_->foreground.green()
                                     : session_->foreground.blue();
                    p[c] = uchar(std::lround(base[c] * (1 - alpha) + v * alpha));
                }
                p[3] = uchar(std::lround(base[3] * (1 - alpha) + 255 * alpha));
            }
        }
    if (!paintMask() && session_->tool != Tool::Heal) {
        layer->metadata.remove("text");
        layer->metadata.remove("shape");
    }
    emit edited();
}
void Canvas::finishSelection() {
    QImage mask(document_->size(), QImage::Format_Grayscale8);
    require(!mask.isNull(), "Not enough memory for selection");
    mask.fill(0);
    QPainter p(&mask);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    auto r = QRectF(start_, last_).normalized();
    if (session_->tool == Tool::EllipseSelect)
        p.drawEllipse(r);
    else if (session_->tool == Tool::Lasso) {
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
    replaceSelection(mask, "Change Selection");
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || dragging_)
        return;
    setFocus();
    start_ = last_ = toDocument(e->position());
    try {
        controller().press(e);
    } catch (const std::exception &ex) {
        cancelInteraction();
        emit error(QString::fromUtf8(ex.what()));
    }
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    if (!dragging_)
        return;
    try {
        controller().move(e);
    } catch (const std::exception &ex) {
        cancelInteraction();
        emit error(QString::fromUtf8(ex.what()));
    }
}
void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (!dragging_ || e->button() != Qt::LeftButton)
        return;
    last_ = toDocument(e->position());
    try {
        controller().release(e);
    } catch (const std::exception &ex) {
        emit error(QString::fromUtf8(ex.what()));
    }
    dragging_ = false;
    original_ = {};
    coverage_ = {};
    blurred_ = {};
    warp_.reset();
    update();
}
void Canvas::keyPressEvent(QKeyEvent *e) {
    if (controller().keyPress(e))
        return;
    if (e->key() == Qt::Key_Escape) {
        controller().cancel();
    } else if (e->key() == Qt::Key_Space && !e->isAutoRepeat() && !dragging_) {
        temporaryPan_ = true;
        setCursor(Qt::OpenHandCursor);
    } else if (e->key() == Qt::Key_X && e->modifiers() == Qt::NoModifier) {
        std::swap(session_->foreground, session_->background);
        emit sessionChanged();
    } else if (e->key() == Qt::Key_D && e->modifiers() == Qt::NoModifier) {
        session_->foreground = Qt::black;
        session_->background = Qt::white;
        emit sessionChanged();
    } else {
        QWidget::keyPressEvent(e);
        return;
    }
    e->accept();
}
void Canvas::keyReleaseEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        cancelInteraction();
        temporaryPan_ = false;
        setCursor(session_->tool == Tool::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
        e->accept();
    } else if (!controller().keyRelease(e)) {
        QWidget::keyReleaseEvent(e);
    }
}
void Canvas::focusOutEvent(QFocusEvent *e) {
    cancelInteraction();
    temporaryPan_ = false;
    setCursor(session_->tool == Tool::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
    QWidget::focusOutEvent(e);
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
