// SPDX-License-Identifier: MIT
#include "editor.h"
#include "effects.h"
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
    QDialog dialog(this);
    dialog.setWindowTitle("Layer Effect: " + key);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    layout->addLayout(form);
    auto preview = new QLabel;
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumSize(256, 160);
    layout->addWidget(preview);
    auto source = layer->image.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    double scale = double(source.width()) / layer->image.width();
    QTimer timer;
    timer.setSingleShot(true);
    auto refresh = [&] {
        try {
            auto scaled = e;
            for (auto field : {"size", "blur", "distance"})
                if (scaled.contains(QLatin1String(field)))
                    scaled[QLatin1String(field)] =
                        scaled.value(QLatin1String(field)).toDouble() * scale;
            auto image = renderEffects(source, {{key, scaled}}).image;
            preview->setPixmap(QPixmap::fromImage(
                image.scaled(360, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        } catch (const std::exception &error) {
            preview->setText(QString::fromUtf8(error.what()));
        }
    };
    connect(&timer, &QTimer::timeout, &dialog, refresh);
    auto number = [&](const char *field, const QString &label, double initial, double low,
                      double high) {
        auto spin = new QDoubleSpinBox;
        spin->setRange(low, high);
        spin->setDecimals(2);
        spin->setValue(e.value(QLatin1String(field)).toDouble(initial));
        e[QLatin1String(field)] = spin->value();
        form->addRow(label, spin);
        connect(spin, &QDoubleSpinBox::valueChanged, &dialog, [&, field](double v) {
            e[QLatin1String(field)] = v;
            timer.start(100);
        });
    };
    auto check = [&](const char *field, const QString &label, bool initial) {
        auto box = new QCheckBox(label);
        box->setChecked(e.value(QLatin1String(field)).toBool(initial));
        e[QLatin1String(field)] = box->isChecked();
        form->addRow(box);
        connect(box, &QCheckBox::toggled, &dialog, [&, field](bool v) {
            e[QLatin1String(field)] = v;
            timer.start(100);
        });
    };
    check("enabled", "Enabled", true);
    number("opacity", "Opacity", 1, 0, 1);
    if (key == "stroke") {
        number("size", "Width", 4, 0, 500);
        check("inside", "Inside", false);
    }
    if (glow)
        number("size", "Size", inner ? 10 : 20, 0, 500);
    if (shadow) {
        number("angle", "Light angle", 90, -360, 360);
        number("distance", "Distance", inner ? 10 : 20, 0, 5000);
        number("blur", "Blur", inner ? 10 : 20, 0, 500);
    }
    auto color = new QPushButton("Color…");
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
            timer.start(100);
        }
    });
    bool remove = false;
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto removeButton = buttons->addButton("Remove", QDialogButtonBox::DestructiveRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(removeButton, &QPushButton::clicked, &dialog, [&] {
        remove = true;
        dialog.accept();
    });
    refresh();
    if (dialog.exec() != QDialog::Accepted)
        return;
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
