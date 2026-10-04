// SPDX-License-Identifier: MIT
#include "canvas_tools.h"
#include "editable_layers.h"
#include "filters.h"
#include "render.h"
#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDragEnterEvent>
#include <QDropEvent>
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
class PanTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *e) override {
        c.dragging_ = true;
        c.start_ = c.last_ = e->position();
        return;
    }
    void move(QMouseEvent *e) override {
        c.pan_ += e->position() - c.last_;
        c.last_ = e->position();
        c.update();
        return;
    }
};
class ZoomTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *event) override {
        c.zoomTo(c.zoom * (event->modifiers() & Qt::AltModifier ? .5 : 2), event->position());
    }
};
class EyedropperTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    // Samples only the requested full-resolution region.
    void press(QMouseEvent *) override {
        if (QRect(QPoint(), c.document_->size()).contains(c.start_.toPoint()))
            emit c.colorPicked(c.sampleColor(c.start_, c.session().pickerSize));
    }
};
class WandTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *e) override {
        auto composite = c.fullComposite();
        if (!c.session().wandMerged) {
            Document isolated = *c.document_;
            for (auto &layer : isolated.layers)
                layer.metadata["isVisible"] = layer.id() == isolated.activeId() || layer.group();
            composite = renderDocument(isolated);
        }
        auto modifiers = e->modifiers();
        if (modifiers == Qt::NoModifier) {
            if (c.session().selectionMode == 1)
                modifiers = Qt::ShiftModifier;
            if (c.session().selectionMode == 2)
                modifiers = Qt::AltModifier;
            if (c.session().selectionMode == 3)
                modifiers = Qt::ShiftModifier | Qt::AltModifier;
        }
        int x = int(c.start_.x()), y = int(c.start_.y());
        if (!composite.valid(x, y))
            return;
        QImage next(composite.size(), QImage::Format_Grayscale8);
        require(!next.isNull(), "Not enough memory for selection");
        std::vector<uchar> packed(size_t(next.width()) * next.height());
        auto result = wand_mask(
            composite.constBits(), size_t(composite.width()), size_t(composite.height()),
            size_t(composite.bytesPerLine()), size_t(x), size_t(y), c.session().wandSampleSize / 2,
            float(c.session().wandTolerance), c.session().wandContiguous, packed.data());
        require(result >= 0, "Magic wand ran out of memory");
        for (int row = 0; row < next.height(); ++row)
            std::copy_n(packed.data() + size_t(row) * next.width(), next.width(),
                        next.scanLine(row));
        if (!c.session().selection.isNull() && (modifiers & (Qt::ShiftModifier | Qt::AltModifier)))
            for (int row = 0; row < next.height(); ++row)
                for (int col = 0; col < next.width(); ++col)
                    next.scanLine(row)[col] =
                        ((modifiers & (Qt::ShiftModifier | Qt::AltModifier)) ==
                         (Qt::ShiftModifier | Qt::AltModifier))
                            ? uchar(int(c.session().selection.constScanLine(row)[col]) *
                                    int(next.constScanLine(row)[col]) / 255)
                        : (modifiers & Qt::AltModifier)
                            ? uchar(c.session().selection.constScanLine(row)[col] *
                                    (1 - next.constScanLine(row)[col] / 255.0))
                            : std::max(c.session().selection.constScanLine(row)[col],
                                       next.constScanLine(row)[col]);
        c.replaceSelection(next, "Magic Wand Selection");
        return;
    }
};
class TextTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *) override {
        bool ok = false;
        auto text = QInputDialog::getMultiLineText(&c, "Text Layer", "Text", {}, &ok);
        if (!ok || text.isEmpty())
            return;
        emit c.editStarted();
        QJsonObject style{{"content", text},
                          {"fontName", c.session().textFont},
                          {"fontSize", c.session().textSize},
                          {"red", c.session().foreground.redF()},
                          {"green", c.session().foreground.greenF()},
                          {"blue", c.session().foreground.blueF()},
                          {"alignment", c.session().textAlignment},
                          {"tracking", c.session().textTracking},
                          {"leading", c.session().textLeading}};
        auto image = renderText(style);
        auto id = c.document_->addImage(text.left(30), image);
        auto l = c.document_->find(id);
        l->move(c.start_);
        l->metadata["text"] = style;
        emit c.editFinished("Text Layer");
        c.refresh();
        return;
    }
};
class SelectTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *e) override {
        c.selectLayerAt(c.start_, e->modifiers() & Qt::ShiftModifier);
    }
};
class MoveTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *e) override {
        if (c.session().autoSelect)
            c.selectLayerAt(c.start_, e->modifiers() & Qt::ShiftModifier);
        require(c.document_->active(), "Select a layer first");
        c.transformBefore_ = *c.document_->active();

        emit c.editStarted();
        c.beginLayerEdit(c.document_->activeId());
        c.dragging_ = true;
        return;
    }
    void move(QMouseEvent *e) override {
        auto point = c.toDocument(e->position());

        auto layer = c.document_->active();
        if (layer) {
            auto delta = point - c.start_;
            if (e->modifiers() & Qt::ShiftModifier) {
                if (std::abs(delta.x()) > std::abs(delta.y()))
                    delta.setY(0);
                else
                    delta.setX(0);
            }
            auto target = c.transformTarget(c.transformBefore_);
            const auto source = target.image.isNull() ? QSize(1, 1) : target.image.size();
            if (!(e->modifiers() & Qt::ControlModifier)) {
                auto frame =
                    target.placement(source).mapRect(QRectF(QPointF(), source)).translated(delta);
                auto adjustment =
                    snapBounds(*c.document_, c.session().view, frame, c.zoom, {layer->id()});
                if (e->modifiers() & Qt::ShiftModifier) {
                    if (delta.x() == 0)
                        adjustment.setX(0);
                    else
                        adjustment.setY(0);
                }
                delta += adjustment;
            }
            layer->metadata = c.transformBefore_.metadata;
            if (c.paintMask() && !layer->mask.isNull()) {
                target.move(delta);
                layer->metadata["maskPlacement"] = target.transform();
            } else
                layer->move(delta);
            emit c.edited();
            c.refresh();
        }

        c.last_ = point;
    }
    void release(QMouseEvent *) override {
        emit c.editFinished("Move Layer");
    }
};
class SelectionTool : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *e) override {
        const bool polygon = c.session().tool == Tool::Lasso && c.session().polygonalLasso;
        if (polygon && !c.polygonPoints_.isEmpty()) {
            if (c.polygonPoints_.size() >= 3 &&
                QLineF(c.start_, c.polygonPoints_.front()).length() * c.zoom <= 6) {
                finishPolygon();
                return;
            }
            if (c.start_ != c.polygonPoints_.back())
                c.polygonPoints_.append(c.start_);
            c.update();
            return;
        }
        c.dragging_ = !polygon;
        if (polygon)
            c.polygonPoints_.append(c.start_);
        c.priorSelection_ = c.session().selection;
        c.selectionModifiers_ = e->modifiers();
        if (c.selectionModifiers_ == Qt::NoModifier) {
            if (c.session().selectionMode == 1)
                c.selectionModifiers_ = Qt::ShiftModifier;
            if (c.session().selectionMode == 2)
                c.selectionModifiers_ = Qt::AltModifier;
            if (c.session().selectionMode == 3)
                c.selectionModifiers_ = Qt::ShiftModifier | Qt::AltModifier;
        }
        c.lasso_ = QPainterPath(c.start_);
        return;
    }
    void move(QMouseEvent *e) override {
        c.last_ = c.toDocument(e->position());
        if (c.session().tool == Tool::Lasso && !c.session().polygonalLasso)
            c.lasso_.lineTo(c.last_);
        c.update();
    }
    void release(QMouseEvent *) override {
        c.finishSelection();
        c.update();
        return;
    }
    void doubleClick(QMouseEvent *) override { finishPolygon(); }
    bool keyPress(QKeyEvent *e) override {
        if (c.polygonPoints_.isEmpty())
            return false;
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            finishPolygon();
            return true;
        }
        if (e->key() == Qt::Key_Backspace) {
            c.polygonPoints_.removeLast();
            c.update();
            return true;
        }
        return false;
    }
    void paintOverlay(QPainter &p) override {
        if (c.polygonPoints_.isEmpty()) {
            c.drawGesture(p);
            return;
        }
        QPainterPath path(c.polygonPoints_.front());
        for (int i = 1; i < c.polygonPoints_.size(); ++i)
            path.lineTo(c.polygonPoints_[i]);
        path.lineTo(c.last_);
        QTransform map;
        map.translate(c.canvasRect().left(), c.canvasRect().top());
        map.scale(c.zoom, c.zoom);
        auto overlay = map.map(path);
        p.setPen(QPen(Qt::black, 2));
        p.drawPath(overlay);
        p.setPen(QPen(Qt::white, 1, Qt::DashLine));
        p.drawPath(overlay);
        for (auto point : c.polygonPoints_)
            p.drawRect(QRectF(map.map(point) - QPointF(2, 2), QSizeF(4, 4)));
    }
  private:
    void finishPolygon() {
        if (c.polygonPoints_.size() < 3)
            return;
        c.lasso_ = QPainterPath(c.polygonPoints_.front());
        for (int i = 1; i < c.polygonPoints_.size(); ++i)
            c.lasso_.lineTo(c.polygonPoints_[i]);
        c.finishSelection();
        c.polygonPoints_.clear();
        c.lasso_ = {};
        c.update();
    }
};
class ShapeTool : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *e) override {
        c.start_ = {c.snapValue(c.start_.x(), false, e->modifiers()),
                    c.snapValue(c.start_.y(), true, e->modifiers())};
        c.last_ = c.start_;
        emit c.editStarted();
        c.dragging_ = true;
        return;
    }
    void move(QMouseEvent *e) override {
        c.last_ = c.toDocument(e->position());
        c.last_ = {c.snapValue(c.last_.x(), false, e->modifiers()),
                   c.snapValue(c.last_.y(), true, e->modifiers())};
        if (e->modifiers() & Qt::ShiftModifier) {
            auto delta = c.last_ - c.start_;
            if (c.session().tool == Tool::Line) {
                auto angle =
                    std::round(std::atan2(delta.y(), delta.x()) / (3.141592653589793 / 12)) *
                    (3.141592653589793 / 12);
                auto length = std::hypot(delta.x(), delta.y());
                c.last_ = c.start_ + QPointF(std::cos(angle), std::sin(angle)) * length;
            } else {
                const auto side = std::max(std::abs(delta.x()), std::abs(delta.y()));
                c.last_ =
                    c.start_ + QPointF(delta.x() < 0 ? -side : side, delta.y() < 0 ? -side : side);
            }
        }
        c.update();
    }
    void release(QMouseEvent *e) override {
        move(e);
        auto r = QRectF(c.start_, c.last_).normalized();
        if (c.session().tool != Tool::Line && (r.width() < 1 || r.height() < 1)) {
            emit c.editCanceled();
            return;
        }
        if (c.session().tool == Tool::Line) {
            auto bounds =
                r.adjusted(-c.session().shapeLineWidth / 2, -c.session().shapeLineWidth / 2,
                           c.session().shapeLineWidth / 2, c.session().shapeLineWidth / 2);
            QJsonObject style{{"kind", "Line"},
                              {"red", c.session().foreground.redF()},
                              {"green", c.session().foreground.greenF()},
                              {"blue", c.session().foreground.blueF()},
                              {"cornerRadius", 0},
                              {"lineWidth", c.session().shapeLineWidth},
                              {"start", QJsonArray{(c.start_.x() - bounds.x()) / bounds.width(),
                                                   (c.start_.y() - bounds.y()) / bounds.height()}},
                              {"end", QJsonArray{(c.last_.x() - bounds.x()) / bounds.width(),
                                                 (c.last_.y() - bounds.y()) / bounds.height()}}};
            auto image = renderShape(style, bounds.size().toSize());
            auto id = c.document_->addImage("Line", image);
            c.document_->find(id)->setBounds(bounds);
            c.document_->find(id)->metadata["shape"] = style;
            emit c.editFinished("Line Layer");
            c.refresh();
            return;
        }
        QImage image(QSize(int(std::ceil(r.width())), int(std::ceil(r.height()))),
                     QImage::Format_RGBA8888_Premultiplied);
        require(!image.isNull(), "Not enough memory for shape");
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(c.session().foreground);
        if (c.session().tool == Tool::Ellipse)
            p.drawEllipse(QRectF(QPointF(), image.size()));
        else
            p.drawRoundedRect(QRectF(QPointF(), image.size()), c.session().shapeCornerRadius,
                              c.session().shapeCornerRadius);
        p.end();
        auto id = c.document_->addImage(c.session().tool == Tool::Ellipse ? "Ellipse" : "Rectangle",
                                        image);
        auto layer = c.document_->find(id);
        layer->setBounds(r);
        layer->metadata["shape"] =
            QJsonObject{{"kind", c.session().tool == Tool::Ellipse ? "Ellipse" : "Rectangle"},
                        {"red", c.session().foreground.redF()},
                        {"green", c.session().foreground.greenF()},
                        {"blue", c.session().foreground.blueF()},
                        {"cornerRadius", c.session().shapeCornerRadius}};
        emit c.editFinished("Shape Layer");
        c.refresh();
        return;
    }
    void paintOverlay(QPainter &p) override {
        c.drawGesture(p);
    }
};
class PaintTool : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *e) override {
        if (c.session().tool == Tool::Clone && (e->modifiers() & Qt::AltModifier)) {
            c.cloneSource_ = c.start_;
            c.cloneReady_ = true;
            c.cloneStrokeReady_ = false;
            return;
        }
        c.beginPaint(e);
    }
    void move(QMouseEvent *e) override {
        c.continuePaint(e);
    }
    void release(QMouseEvent *e) override {
        c.finishPaint(e);
    }
    void paintOverlay(QPainter &p) override {
        c.drawGesture(p);
    }
};
class BrushTool final : public PaintTool {
  public:
    using PaintTool::PaintTool;
};
class EraseTool final : public PaintTool {
  public:
    using PaintTool::PaintTool;
};
class CloneTool final : public PaintTool {
  public:
    using PaintTool::PaintTool;
};
class HealTool final : public PaintTool {
  public:
    using PaintTool::PaintTool;
};
class BlurTool final : public PaintTool {
  public:
    using PaintTool::PaintTool;
};
class SmudgeTool final : public PaintTool {
  public:
    using PaintTool::PaintTool;
};
class LiquifyTool final : public PaintTool {
  public:
    using PaintTool::PaintTool;
};
class GradientTool final : public PaintTool {
  public:
    using PaintTool::PaintTool;
};
class RectangleSelectTool final : public SelectionTool {
  public:
    using SelectionTool::SelectionTool;
};
class EllipseSelectTool final : public SelectionTool {
  public:
    using SelectionTool::SelectionTool;
};
class LassoTool final : public SelectionTool {
  public:
    using SelectionTool::SelectionTool;
};
class RectangleTool final : public ShapeTool {
  public:
    using ShapeTool::ShapeTool;
};
class EllipseTool final : public ShapeTool {
  public:
    using ShapeTool::ShapeTool;
};
class LineTool final : public ShapeTool {
  public:
    using ShapeTool::ShapeTool;
};
std::unique_ptr<CanvasTool> makeCanvasTool(Tool kind, Canvas &canvas) {
    switch (kind) {
    case Tool::Select:
        return std::make_unique<SelectTool>(canvas);
    case Tool::Move:
        return std::make_unique<MoveTool>(canvas);
    case Tool::Pan:
        return std::make_unique<PanTool>(canvas);
    case Tool::Zoom:
        return std::make_unique<ZoomTool>(canvas);
    case Tool::Eyedropper:
        return std::make_unique<EyedropperTool>(canvas);
    case Tool::Wand:
        return std::make_unique<WandTool>(canvas);
    case Tool::Text:
        return std::make_unique<TextTool>(canvas);
    case Tool::Brush:
        return std::make_unique<BrushTool>(canvas);
    case Tool::Erase:
        return std::make_unique<EraseTool>(canvas);
    case Tool::Clone:
        return std::make_unique<CloneTool>(canvas);
    case Tool::Heal:
        return std::make_unique<HealTool>(canvas);
    case Tool::Blur:
        return std::make_unique<BlurTool>(canvas);
    case Tool::Smudge:
        return std::make_unique<SmudgeTool>(canvas);
    case Tool::Liquify:
        return std::make_unique<LiquifyTool>(canvas);
    case Tool::Gradient:
        return std::make_unique<GradientTool>(canvas);
    case Tool::RectangleSelect:
        return std::make_unique<RectangleSelectTool>(canvas);
    case Tool::EllipseSelect:
        return std::make_unique<EllipseSelectTool>(canvas);
    case Tool::Lasso:
        return std::make_unique<LassoTool>(canvas);
    case Tool::Crop:
        return makeCropTool(canvas);
    case Tool::Rectangle:
        return std::make_unique<RectangleTool>(canvas);
    case Tool::Ellipse:
        return std::make_unique<EllipseTool>(canvas);
    case Tool::Line:
        return std::make_unique<LineTool>(canvas);
    }
    throw Error("Unknown canvas tool");
}
} // namespace compositor
