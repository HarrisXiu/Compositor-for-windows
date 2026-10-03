// SPDX-License-Identifier: MIT
#pragma once
#include "canvas.h"
class QKeyEvent;
class QMouseEvent;
class QPainter;

namespace compositor {
// QWidget routes events once; each tool owns its interaction policy.
class CanvasTool {
  public:
    explicit CanvasTool(Canvas &canvas) : c(canvas) {}
    virtual ~CanvasTool() = default;
    virtual void press(QMouseEvent *) {}
    virtual void move(QMouseEvent *) {}
    virtual void release(QMouseEvent *) {}
    virtual bool keyPress(QKeyEvent *) {
        return false;
    }
    virtual bool keyRelease(QKeyEvent *) {
        return false;
    }
    virtual void paintOverlay(QPainter &) {}
    virtual void cancel() {
        c.cancelInteraction();
    }

  protected:
    Canvas &c;
};
std::unique_ptr<CanvasTool> makeCanvasTool(Tool kind, Canvas &canvas);
std::unique_ptr<CanvasTool> makeCropTool(Canvas &canvas);
} // namespace compositor
