// SPDX-License-Identifier: MIT
#pragma once
#include <QJsonArray>
#include <QWidget>
#include <functional>
namespace compositor {
class CurveEditor : public QWidget {
  public:
    explicit CurveEditor(QWidget *parent = nullptr);
    QJsonArray points;
    std::function<void(const QJsonArray &)> changed;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

  private:
    int dragging_ = -1;
    QPointF toValue(QPointF p) const;
    QPointF toWidget(QPointF p) const;
};
} // namespace compositor
