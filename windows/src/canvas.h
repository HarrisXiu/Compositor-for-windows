// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include "editor_session.h"
#include "warp_brush.h"
#include <QPainterPath>
#include <QWidget>
#include <memory>

namespace compositor {
class CanvasTool;
class PaintTool;
class SelectionTool;
class ShapeTool;
class MoveTool;
class PanTool;
class EyedropperTool;
class WandTool;
class TextTool;
class Canvas : public QWidget {
    Q_OBJECT
  public:
    explicit Canvas(Document *document, QWidget *parent = nullptr);
    Canvas(Document *document, EditorSession *session, QWidget *parent);
    ~Canvas() override;
    EditorSession &session() {
        return *session_;
    }
    const EditorSession &session() const {
        return *session_;
    }
    bool paintMask() const {
        return session_->target == EditTarget::Mask;
    }
    double zoom = 1;
    void refresh();
    void fit();
    void clearSelection();
    void selectAll();
    void invertSelection();
    void replaceSelection(const QImage &mask, const QString &label);
    void featherSelection(double radius);
    void cancelInteraction();
    QRect selectionBounds() const;
    void setTool(Tool value);
    QImage selectionForLayer(const Layer &layer) const;
  signals:
    void editStarted();
    void editFinished(const QString &label);
    void editCanceled();
    void edited();
    void colorPicked(QColor color);
    void filesDropped(const QStringList &paths);
    void selectionChanged();
    void selectionEdited(const QString &label, const QImage &before, const QImage &after);
    void sessionChanged();
    void error(const QString &message);

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void focusOutEvent(QFocusEvent *) override;

  private:
    Document *document_;
    EditorSession fallbackSession_;
    EditorSession *session_;
    std::unique_ptr<CanvasTool> controller_;
    Tool controllerKind_ = Tool::Move;
    bool temporaryPan_ = false;
    QImage preview_, original_, coverage_, priorSelection_, blurred_;
    std::unique_ptr<WarpBrush> warp_;
    bool warpChanged_ = false;
    QPointF pan_, start_, last_, cloneSource_, cloneOffset_;
    bool dragging_ = false, cloneReady_ = false;
    Qt::KeyboardModifiers selectionModifiers_{};
    QPainterPath lasso_;
    QPainterPath selectionOutline_;
    qint64 selectionOutlineKey_ = 0;
    QRectF canvasRect() const;
    QPointF toDocument(QPointF point) const;
    void dab(QPointF point);
    void finishSelection();
    void rebuildPreview();
    CanvasTool &controller();
    void beginPaint(QMouseEvent *event);
    void continuePaint(QMouseEvent *event);
    void finishPaint(QMouseEvent *event);
    void drawGesture(QPainter &painter);
    friend class PaintTool;
    friend class SelectionTool;
    friend class ShapeTool;
    friend class MoveTool;
    friend class PanTool;
    friend class EyedropperTool;
    friend class WandTool;
    friend class TextTool;
};
} // namespace compositor
