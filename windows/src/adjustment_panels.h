// SPDX-License-Identifier: MIT
#pragma once
#include "adjustment_tools.h"
#include <QImage>
#include <QJsonObject>
#include <QWidget>
#include <functional>

namespace compositor {
// Levels: the original pixels' histogram for the chosen channel, with the input black, gamma and
// white handles under it and the output black and white handles under a gray ramp. Dragging a
// handle reports the new value; the dialog's fields hold the values themselves.
class LevelsGraph final : public QWidget {
  public:
    explicit LevelsGraph(QWidget *parent = nullptr);
    Histogram histogram{};
    bool histogramReady = false;
    int channel = 0; // RGB, red, green, blue.
    double black = 0, gamma = 1, white = 255, outputBlack = 0, outputWhite = 255;
    // inputBlack, gamma, inputWhite, outputBlack or outputWhite, and its new value.
    std::function<void(const QString &key, double value)> changed;
    // Where the gamma handle sits: the input that becomes middle gray.
    double gammaPosition() const;
    // The handle positions along the widget, for tests.
    QRectF histogramRect() const;
    QRectF inputStrip() const;
    QRectF outputStrip() const;
    double xOf(double value) const;
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

  private:
    QString dragging_;
    void dragTo(double x);
};

// Hue/Saturation's two spectrum bars: the hues as they are, the selected range's band handles,
// and the hues as the adjustment leaves them. The outer handles are where the band fades out,
// the inner ones where it is at full strength.
class HueSpectrum final : public QWidget {
  public:
    explicit HueSpectrum(QWidget *parent = nullptr);
    QJsonObject band;
    // 360 × 1: each hue as the current settings leave it.
    QImage after;
    std::function<void(const QJsonObject &band)> changed;
    double xOf(double degrees) const;
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

  private:
    int dragging_ = -1;
    double degreesAt(double x) const;
};
// A color grading wheel: hue around it (red at the right, counterclockwise), saturation outward
// from the center. Dragging sets both; double-clicking resets the wheel.
class ColorWheel final : public QWidget {
  public:
    explicit ColorWheel(QWidget *parent = nullptr);
    double hue = 0, saturation = 0;
    std::function<void(double hue, double saturation)> changed;
    // Where (hue, saturation) is drawn, in widget coordinates.
    QPointF pointOf(double hue, double saturation) const;
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

  private:
    double radius() const;
    void setFrom(QPointF position);
};
} // namespace compositor
