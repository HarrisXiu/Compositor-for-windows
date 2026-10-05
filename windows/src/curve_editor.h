// SPDX-License-Identifier: MIT
#pragma once
#include <QJsonArray>
#include <QWidget>
#include <array>
#include <functional>
namespace compositor {
class CurveEditor : public QWidget {
  public:
    explicit CurveEditor(QWidget *parent = nullptr);
    QJsonArray points;
    std::function<void(const QJsonArray &)> changed;
    // Drawn behind the curve when set: the channel's histogram of the original pixels.
    std::array<double, 256> histogram{};
    bool showHistogram = false;
    QColor histogramColor = QColor(120, 120, 120);
    // The point last clicked or dragged, -1 for none; reported as it changes.
    int selected = -1;
    std::function<void(int)> selectionChanged;
    // Removes the selected point unless it is an end point.
    void removeSelected();
    void resetCurve();

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
