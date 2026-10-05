// SPDX-License-Identifier: MIT
#pragma once
#include <QLabel>
#include <QWidget>
#include <functional>

class QDoubleSpinBox;
class QFormLayout;
class QSlider;

namespace compositor {
// A number you can set three ways: a slider, a field, or by dragging its label sideways (Shift for
// fine steps, Ctrl for coarse ones). Double-clicking the label or the slider puts it back to its
// default. The field is an ordinary spin box, so `valueChanged` and `setValue` work on it as before.
class ParameterControl final : public QWidget {
    Q_OBJECT
  public:
    ParameterControl(double low, double high, int decimals, double initial, double reset,
                     QWidget *parent = nullptr);
    QDoubleSpinBox *spin() const {
        return spin_;
    }
    QSlider *slider() const {
        return slider_;
    }
    double value() const;
    double resetValue() const {
        return reset_;
    }
    void setValue(double value);
    void reset() {
        setValue(reset_);
    }
    // Colors the slider's groove from the first to the last stop, for sliders like Temperature.
    void setTrack(const QList<QColor> &stops);
    // The change in value per pixel of horizontal drag on the label.
    double scrubSensitivity() const;

  signals:
    void valueChanged(double value);
    // The pointer went down on the slider or label and came back up: one adjustment finished.
    void adjustmentFinished();

  protected:
    bool eventFilter(QObject *, QEvent *) override;

  private:
    QDoubleSpinBox *spin_;
    QSlider *slider_;
    double reset_;
    int scale_;
    void syncSlider();
};

// The label of a ParameterControl: dragging it scrubs the value.
class ScrubLabel final : public QLabel {
    Q_OBJECT
  public:
    ScrubLabel(const QString &text, ParameterControl *control, QWidget *parent = nullptr);

  protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

  private:
    ParameterControl *control_;
    double startValue_ = 0;
    double startX_ = 0;
    bool scrubbing_ = false;
};

// Adds a row of `label` (scrubbable) and the control to a form. Returns the control.
ParameterControl *addParameter(QFormLayout *form, const QString &label, ParameterControl *control,
                               const QString &objectName = {});
} // namespace compositor
