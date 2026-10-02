// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <vector>
namespace compositor {
enum class WarpMode { Smudge, Liquify };
// CPU port of the stroke algorithms in Compositor/Document/SmudgeLiquify.swift.
class WarpBrush {
  public:
    WarpBrush(const QImage &image, const QTransform &placement, QSize canvas, WarpMode mode,
              double diameter, double hardness, double strength);
    bool append(QPointF point);
    QImage result(const QImage &original, const QTransform &placement,
                  const QImage &selection) const;

  private:
    QImage pixels_, coverage_;
    WarpMode mode_;
    double diameter_, hardness_, strength_;
    QPointF last_;
    bool started_ = false;
    std::vector<float> carried_, scratch_;
    int radius() const;
    double weight(double distance) const;
    void pickup(QPointF point);
    void smudge(QPointF point);
    void push(QPointF from, QPointF to);
};
} // namespace compositor
