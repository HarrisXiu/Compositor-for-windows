// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include "warp_brush.h"
#include <QPainterPath>
#include <QWidget>
#include <memory>

namespace compositor {
enum class Tool {
    Move,
    Brush,
    Erase,
    RectangleSelect,
    EllipseSelect,
    Lasso,
    Wand,
    Gradient,
    Rectangle,
    Ellipse,
    Line,
    Text,
    Eyedropper,
    Clone,
    Heal,
    Blur,
    Smudge,
    Liquify,
    Pan,
    Crop
};

class Canvas : public QWidget {
    Q_OBJECT
  public:
    explicit Canvas(Document *document, QWidget *parent = nullptr);
    Tool tool = Tool::Move;
    QColor color = QColor(68, 157, 245);
    double brushSize = 32;
    double hardness = 0.8;
    double brushOpacity = 1;
    double blurRadius = 5;
    bool paintMask = false;
    double wandTolerance = 32;
    bool wandContiguous = true;
    double zoom = 1;
    QImage selection;
    void refresh();
    void fit();
    void clearSelection();
    void selectAll();
    void invertSelection();
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

  private:
    Document *document_;
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
};
} // namespace compositor
