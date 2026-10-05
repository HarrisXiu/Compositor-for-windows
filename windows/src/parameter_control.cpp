// SPDX-License-Identifier: MIT
#include "parameter_control.h"
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <algorithm>
#include <cmath>

namespace compositor {
namespace {
int sliderScale(double low, double high, int decimals) {
    int scale = 1;
    for (int i = 0; i < decimals; ++i)
        scale *= 10;
    while (scale > 1 && (high - low) * scale > 200000)
        scale /= 10;
    return scale;
}
} // namespace

ParameterControl::ParameterControl(double low, double high, int decimals, double initial,
                                   double reset, QWidget *parent)
    : QWidget(parent), reset_(reset), scale_(sliderScale(low, high, decimals)) {
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setRange(int(std::lround(low * scale_)), int(std::lround(high * scale_)));
    slider_->setMinimumWidth(120);
    slider_->setFocusPolicy(Qt::StrongFocus);
    slider_->setToolTip("Drag to adjust. Double-click to reset.");
    slider_->installEventFilter(this);
    spin_ = new QDoubleSpinBox;
    spin_->setRange(low, high);
    spin_->setDecimals(decimals);
    spin_->setMinimumWidth(76);
    spin_->setValue(initial);
    layout->addWidget(slider_, 1);
    layout->addWidget(spin_);
    syncSlider();
    connect(slider_, &QSlider::valueChanged, this, [this](int position) {
        spin_->setValue(double(position) / scale_);
    });
    connect(slider_, &QSlider::sliderReleased, this, &ParameterControl::adjustmentFinished);
    connect(spin_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        syncSlider();
        emit valueChanged(value);
    });
    connect(spin_, &QDoubleSpinBox::editingFinished, this, &ParameterControl::adjustmentFinished);
    setFocusProxy(spin_);
}
double ParameterControl::value() const {
    return spin_->value();
}
void ParameterControl::setValue(double value) {
    spin_->setValue(value);
}
void ParameterControl::syncSlider() {
    QSignalBlocker block(slider_);
    slider_->setValue(int(std::lround(spin_->value() * scale_)));
}
double ParameterControl::scrubSensitivity() const {
    const double step = std::pow(10.0, -spin_->decimals());
    return std::max(step, (spin_->maximum() - spin_->minimum()) / 400);
}
void ParameterControl::setTrack(const QList<QColor> &stops) {
    if (stops.size() < 2)
        return;
    QString gradient = "qlineargradient(x1:0,y1:0,x2:1,y2:0";
    for (int i = 0; i < stops.size(); ++i)
        gradient += QString(",stop:%1 %2").arg(double(i) / (stops.size() - 1)).arg(stops[i].name());
    gradient += ")";
    slider_->setStyleSheet(
        "QSlider::groove:horizontal{height:6px;border-radius:3px;background:" + gradient +
        ";}QSlider::handle:horizontal{background:#f2f2f2;border:1px solid #555;width:12px;"
        "margin:-5px 0;border-radius:7px;}");
}
bool ParameterControl::eventFilter(QObject *object, QEvent *event) {
    if (object == slider_ && event->type() == QEvent::MouseButtonDblClick) {
        reset();
        emit adjustmentFinished();
        return true;
    }
    return QWidget::eventFilter(object, event);
}

ScrubLabel::ScrubLabel(const QString &text, ParameterControl *control, QWidget *parent)
    : QLabel(text, parent), control_(control) {
    setCursor(Qt::SplitHCursor);
    setToolTip("Drag sideways to adjust. Shift: fine steps. Ctrl: coarse steps. Double-click to reset.");
    setBuddy(control->spin());
}
void ScrubLabel::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton)
        return QLabel::mousePressEvent(event);
    startValue_ = control_->value();
    startX_ = event->globalPosition().x();
    scrubbing_ = true;
}
void ScrubLabel::mouseMoveEvent(QMouseEvent *event) {
    if (!scrubbing_)
        return;
    double factor = 1;
    if (event->modifiers() & Qt::ShiftModifier)
        factor = .1;
    else if (event->modifiers() & Qt::ControlModifier)
        factor = 10;
    control_->setValue(startValue_ +
                       (event->globalPosition().x() - startX_) * control_->scrubSensitivity() * factor);
}
void ScrubLabel::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && scrubbing_) {
        scrubbing_ = false;
        emit control_->adjustmentFinished();
    }
}
void ScrubLabel::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        scrubbing_ = false;
        control_->reset();
        emit control_->adjustmentFinished();
    }
}

ParameterControl *addParameter(QFormLayout *form, const QString &label, ParameterControl *control,
                               const QString &objectName) {
    if (!objectName.isEmpty()) {
        control->setObjectName(objectName + "Parameter");
        control->spin()->setObjectName(objectName);
    }
    form->addRow(new ScrubLabel(label, control), control);
    return control;
}
} // namespace compositor
