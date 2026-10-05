// SPDX-License-Identifier: MIT
#pragma once
#include <QHash>
#include <QJsonObject>
#include <functional>

class QComboBox;
class QDialog;
class QDoubleSpinBox;
class QFormLayout;
class QVBoxLayout;

namespace compositor {
class CurveEditor;
class EditorPage;
class ParameterControl;

// What the Levels, Curves and Hue/Saturation tools (F2) share with the filter dialog they extend.
struct AdjustmentDialog {
    QDialog &dialog;
    QVBoxLayout *layout;
    QFormLayout *form;
    QJsonObject &settings;
    QHash<QString, QDoubleSpinBox *> &controls;
    QHash<QString, ParameterControl *> &parameters;
    QComboBox *rangeControl;
    // Puts the selected range's or channel's values from `settings` into the fields.
    std::function<void()> reloadRange;
    // Settings changed: preview them.
    std::function<void()> changed;
    EditorPage *page;
    bool asAdjustment, editExisting;
    // The pixel layer filtered, or the adjustment layer edited.
    QString layerId;
};
// Histogram with draggable input/output handles, Auto buttons and black/gray/white eyedroppers
// that sample the original pixels on the canvas.
void addLevelsTools(const AdjustmentDialog &d);
// The channel's histogram behind the curve, the selected point's values, Remove point, Reset.
void addCurvesTools(const AdjustmentDialog &d, CurveEditor *curve, QComboBox *channels);
// Colored slider tracks, the spectrum bars with the range's band handles, "outside this range",
// eyedroppers that set, add to or subtract from the band, and dragging on the canvas to change a
// color's saturation (its hue with Ctrl).
void addHueSaturationTools(const AdjustmentDialog &d);
// Refreshes what the tools show after settings changed (the after-spectrum, slider tracks).
void refreshAdjustmentTools(const AdjustmentDialog &d, const QString &kind);
} // namespace compositor
