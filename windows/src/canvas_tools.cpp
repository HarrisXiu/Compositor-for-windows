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
class EyedropperTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    // Samples the full-size composite under the pointer, rendering just that pixel.
    void press(QMouseEvent *) override {
        const QPoint pixel(int(std::floor(c.start_.x())), int(std::floor(c.start_.y())));
        const auto size = c.document_->size();
        if (!QRect(QPoint(), size).contains(pixel))
            return;
        auto sample = renderArea(*c.document_, {size, QRect(pixel, QSize(1, 1))});
        emit c.colorPicked(sample.pixelColor(0, 0));
    }
};
class WandTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *e) override {
        auto composite = c.fullComposite();
        int x = int(c.start_.x()), y = int(c.start_.y());
        if (!composite.valid(x, y))
            return;
        QImage next(composite.size(), QImage::Format_Grayscale8);
        require(!next.isNull(), "Not enough memory for selection");
        std::vector<uchar> packed(size_t(next.width()) * next.height());
        auto result =
            wand_mask(composite.constBits(), size_t(composite.width()), size_t(composite.height()),
                      size_t(composite.bytesPerLine()), size_t(x), size_t(y), 0,
                      float(c.session().wandTolerance), c.session().wandContiguous, packed.data());
        require(result >= 0, "Magic wand ran out of memory");
        for (int row = 0; row < next.height(); ++row)
            std::copy_n(packed.data() + size_t(row) * next.width(), next.width(),
                        next.scanLine(row));
        if (!c.session().selection.isNull() &&
            (e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier)))
            for (int row = 0; row < next.height(); ++row)
                for (int col = 0; col < next.width(); ++col)
                    next.scanLine(row)[col] =
                        (e->modifiers() & Qt::AltModifier)
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
                          {"fontName", "Segoe UI"},
                          {"fontSize", c.session().brushSize},
                          {"red", c.session().foreground.redF()},
                          {"green", c.session().foreground.greenF()},
                          {"blue", c.session().foreground.blueF()},
                          {"alignment", "Left"},
                          {"tracking", 0},
                          {"leading", 0}};
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
class MoveTool final : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *) override {
        require(c.document_->active(), "Select a layer first");

        emit c.editStarted();
        c.beginLayerEdit(c.document_->activeId());
        c.dragging_ = true;
        return;
    }
    void move(QMouseEvent *e) override {
        auto point = c.toDocument(e->position());

        auto layer = c.document_->active();
        if (layer) {
            layer->move(point - c.last_);
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
        c.dragging_ = true;
        c.priorSelection_ = c.session().selection;
        c.selectionModifiers_ = e->modifiers();
        c.lasso_ = QPainterPath(c.start_);
        return;
    }
    void move(QMouseEvent *e) override {
        c.last_ = c.toDocument(e->position());
        if (c.session().tool == Tool::Lasso)
            c.lasso_.lineTo(c.last_);
        c.update();
    }
    void release(QMouseEvent *) override {
        c.finishSelection();
        c.update();
        return;
    }
    void paintOverlay(QPainter &p) override {
        c.drawGesture(p);
    }
};
class ShapeTool : public CanvasTool {
  public:
    using CanvasTool::CanvasTool;
    void press(QMouseEvent *) override {
        emit c.editStarted();
        c.dragging_ = true;
        return;
    }
    void move(QMouseEvent *e) override {
        c.last_ = c.toDocument(e->position());
        c.update();
    }
    void release(QMouseEvent *) override {
        auto r = QRectF(c.start_, c.last_).normalized();
        if (c.session().tool != Tool::Line && (r.width() < 1 || r.height() < 1)) {
            emit c.editCanceled();
            return;
        }
        if (c.session().tool == Tool::Line) {
            auto bounds = r.adjusted(-c.session().brushSize / 2, -c.session().brushSize / 2,
                                     c.session().brushSize / 2, c.session().brushSize / 2);
            QJsonObject style{{"kind", "Line"},
                              {"red", c.session().foreground.redF()},
                              {"green", c.session().foreground.greenF()},
                              {"blue", c.session().foreground.blueF()},
                              {"cornerRadius", 0},
                              {"lineWidth", c.session().brushSize},
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
            p.drawRect(QRectF(QPointF(), image.size()));
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
                        {"cornerRadius", 0}};
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
    case Tool::Move:
        return std::make_unique<MoveTool>(canvas);
    case Tool::Pan:
        return std::make_unique<PanTool>(canvas);
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
