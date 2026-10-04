// SPDX-License-Identifier: MIT
#pragma once
#include <QGraphicsView>
#include <QJsonObject>
#include <functional>
class QGraphicsProxyWidget;
class QTextEdit;
class QToolBar;

namespace compositor {
class CanvasTextEditor final : public QGraphicsView {
  public:
    CanvasTextEditor(const QJsonObject &style, QWidget *parent);
    ~CanvasTextEditor() override;
    QJsonObject style() const;
    QTextEdit *text() const {
        return text_;
    }
    void place(QTransform sourceToView, QSize sourceSize);
    void setBoxSize(QSizeF size);
    void hideEditor();
    bool resizingFrame() const {
        return handle_ >= 0;
    }
    std::function<void()> changed, fontsChanged, apply, cancel, frameStarted;
    std::function<void(QRectF)> frameChanged;

  protected:
    void drawForeground(QPainter *, const QRectF &) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

  private:
    QJsonObject base_;
    QTextEdit *text_;
    QGraphicsProxyWidget *proxy_;
    QToolBar *toolbar_;
    QTransform placement_, framePlacement_;
    QSize sourceSize_, frameSize_;
    QPointF framePress_;
    int handle_ = -2;
    QVector<QPointF> handles() const;
    void mappingsDialog();
    void updateTypography(double size, double tracking, double leading, const QString &alignment);
};
} // namespace compositor
