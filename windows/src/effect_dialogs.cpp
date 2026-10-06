// SPDX-License-Identifier: MIT
#include "editor.h"
#include "effects.h"
#include "parameter_control.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace compositor {
void EditorWindow::editEffect(const QString &key) {
    auto p = page();
    if (!p)
        return;
    auto layer = p->document.active();
    require(layer && !layer->image.isNull(), "Select an image layer");
    const auto id = layer->id();
    auto all = layer->metadata.value("effects").toObject();
    auto e = all.value(key).toObject();
    bool glow = key == "outerGlow" || key == "innerGlow",
         shadow = key == "shadow" || key == "innerShadow",
         inner = key == "innerShadow" || key == "innerGlow";
    if (e.isEmpty())
        e = {{"enabled", true},
             {"red", glow ? 1 : 0},
             {"green", glow ? 1 : 0},
             {"blue", glow ? 1 : 0},
             {"opacity", glow     ? .75
                         : shadow ? .5
                                  : 1}};
    // As on the Mac, the effect previews on the canvas itself while the panel stays open beside it.
    QDialog dialog(this, Qt::Tool);
    dialog.setObjectName("filterDialog");
    dialog.setWindowTitle("Layer Effect: " + key);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    layout->addLayout(form);
    QTimer timer;
    timer.setSingleShot(true);
    // The layer with the effect as it stands, drawn in the canvas's copy of the document only.
    auto refresh = [&] {
        p->canvas->setLivePreview(
            [id, key, effect = e](Document &d) {
                if (auto *l = d.find(id)) {
                    auto effects = l->metadata.value("effects").toObject();
                    effects[key] = effect;
                    l->metadata["effects"] = effects;
                }
            },
            {}, id);
    };
    connect(&timer, &QTimer::timeout, &dialog, refresh);
    auto number = [&](const char *field, const QString &label, double initial, double low,
                      double high, int decimals) {
        auto parameter = new ParameterControl(
            low, high, decimals, e.value(QLatin1String(field)).toDouble(initial), initial);
        addParameter(form, label, parameter, QString::fromLatin1(field) + "Control");
        e[QLatin1String(field)] = parameter->value();
        connect(parameter->spin(), &QDoubleSpinBox::valueChanged, &dialog, [&, field](double v) {
            e[QLatin1String(field)] = v;
            timer.start(60);
        });
    };
    auto check = [&](const char *field, const QString &label, bool initial) {
        auto box = new QCheckBox(label);
        box->setObjectName(QString::fromLatin1(field) + "Control");
        box->setChecked(e.value(QLatin1String(field)).toBool(initial));
        e[QLatin1String(field)] = box->isChecked();
        form->addRow(box);
        connect(box, &QCheckBox::toggled, &dialog, [&, field](bool v) {
            e[QLatin1String(field)] = v;
            timer.start(60);
        });
    };
    check("enabled", "Enabled", true);
    number("opacity", "Opacity", 1, 0, 1, 2);
    if (key == "stroke") {
        number("size", "Width", 4, 0, 500, 1);
        check("inside", "Inside", false);
    }
    if (glow)
        number("size", "Size", inner ? 10 : 20, 0, 500, 1);
    if (shadow) {
        number("angle", "Light angle", 90, -360, 360, 0);
        number("distance", "Distance", inner ? 10 : 20, 0, 5000, 1);
        number("blur", "Blur", inner ? 10 : 20, 0, 500, 1);
    }
    auto color = new QPushButton("Color…");
    color->setObjectName("effectColor");
    form->addRow(color);
    connect(color, &QPushButton::clicked, &dialog, [&] {
        auto c = QColorDialog::getColor(QColor::fromRgbF(e.value("red").toDouble(),
                                                         e.value("green").toDouble(),
                                                         e.value("blue").toDouble()),
                                        &dialog);
        if (c.isValid()) {
            e["red"] = c.redF();
            e["green"] = c.greenF();
            e["blue"] = c.blueF();
            timer.start(60);
        }
    });
    bool remove = false;
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto removeButton = buttons->addButton("Remove", QDialogButtonBox::DestructiveRole);
    removeButton->setObjectName("removeEffect");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(removeButton, &QPushButton::clicked, &dialog, [&] {
        remove = true;
        dialog.accept();
    });
    refresh();
    const bool accepted = runLiveDialog(dialog);
    timer.stop();
    p->canvas->clearLivePreview();
    if (!accepted)
        return;
    // Nothing else could have been edited meanwhile; check all the same.
    require(page() == p && p->document.activeId() == id, "The layer changed while the dialog was open");
    p->edit(remove ? "Remove Effect" : "Edit Effect", [&](Document &d) {
        auto effects = d.active()->metadata.value("effects").toObject();
        if (remove)
            effects.remove(key);
        else
            effects[key] = e;
        validateEffects(effects);
        if (effects.isEmpty())
            d.active()->metadata.remove("effects");
        else
            d.active()->metadata["effects"] = effects;
    });
}
} // namespace compositor
