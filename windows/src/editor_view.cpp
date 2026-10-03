// SPDX-License-Identifier: MIT
#include "editor.h"
#include "language.h"
#include <QAction>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
namespace compositor {
void EditorWindow::applyViewOptions(const CanvasViewOptions &options) {
    saveCanvasViewOptions(options);
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto p = qobject_cast<EditorPage *>(tabs_->widget(i))) {
            p->session.view = options;
            p->canvas->update();
        }
    syncToolOptions();
}
void EditorWindow::buildViewMenu(QMenu *menu) {
    action(menu, "Zoom In", QKeySequence("Ctrl+="), [this] {
        if (page())
            page()->canvas->zoomTo(page()->canvas->zoom * 1.25);
    });
    action(menu, "Zoom Out", QKeySequence("Ctrl+-"), [this] {
        if (page())
            page()->canvas->zoomTo(page()->canvas->zoom / 1.25);
    });
    auto toggle = [this](QMenu *m, const QString &name, const QString &key,
                         bool CanvasViewOptions::*member) {
        auto a = action(m, name, QKeySequence(key), [] {});
        a->setCheckable(true);
        a->setChecked(loadCanvasViewOptions().*member);
        connect(a, &QAction::toggled, this, [this, member](bool enabled) {
            auto options = loadCanvasViewOptions();
            options.*member = enabled;
            applyViewOptions(options);
        });
    };
    toggle(menu, "Show Transform Controls", "Ctrl+H", &CanvasViewOptions::transformControls);
    toggle(menu, "Show Rulers", "Ctrl+R", &CanvasViewOptions::rulers);
    toggle(menu, "Show Guides", "Ctrl+;", &CanvasViewOptions::guides);
    toggle(menu, "Show Grid", "Ctrl+'", &CanvasViewOptions::grid);
    toggle(menu, "Snap", "Ctrl+Shift+;", &CanvasViewOptions::snap);
    toggle(menu, "Lock Guides", "Ctrl+Alt+;", &CanvasViewOptions::lockGuides);
    auto targets = menu->addMenu("Snap To");
    toggle(targets, "Canvas Edges and Center", "", &CanvasViewOptions::snapCanvas);
    toggle(targets, "Layer Edges and Centers", "", &CanvasViewOptions::snapLayers);
    toggle(targets, "Guides", "", &CanvasViewOptions::snapGuides);
    toggle(targets, "Grid", "", &CanvasViewOptions::snapGrid);
    action(menu, "New Guide…", {}, [this] {
        if (!page() || page()->session.view.lockGuides)
            return;
        QDialog dialog(this);
        dialog.setObjectName("newGuideDialog");
        dialog.setWindowTitle(uiText("New Guide"));
        auto form = new QFormLayout(&dialog);
        auto axis = new QComboBox;
        axis->setObjectName("guideAxis");
        axis->addItems({uiText("Horizontal"), uiText("Vertical")});
        form->addRow(uiText("Orientation"), axis);
        auto position = new QDoubleSpinBox;
        position->setObjectName("guidePosition");
        position->setRange(0, MaxSide);
        position->setDecimals(2);
        position->setSuffix(" px");
        form->addRow(uiText("Position"), position);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted)
            page()->canvas->addGuide(axis->currentIndex() == 0, position->value());
    });
    action(menu, "Clear Guides", {}, [this] {
        if (page())
            page()->canvas->clearGuides();
    });
    action(menu, "Grid and Guide Settings…", {}, [this] {
        auto options = loadCanvasViewOptions();
        QDialog dialog(this);
        dialog.setObjectName("gridSettingsDialog");
        dialog.setWindowTitle(uiText("Grid and Guide Settings"));
        auto form = new QFormLayout(&dialog);
        auto spacing = new QSpinBox;
        spacing->setObjectName("gridSpacing");
        spacing->setRange(2, 4096);
        spacing->setValue(options.gridSpacing);
        form->addRow(uiText("Grid spacing"), spacing);
        auto subdivisions = new QSpinBox;
        subdivisions->setObjectName("gridSubdivisions");
        subdivisions->setRange(1, std::min(64, options.gridSpacing));
        subdivisions->setValue(options.gridSubdivisions);
        form->addRow(uiText("Subdivisions"), subdivisions);
        connect(spacing, &QSpinBox::valueChanged, subdivisions,
                [subdivisions](int value) { subdivisions->setMaximum(std::min(64, value)); });
        auto color = [&](const QString &name, QColor &value) {
            auto button = new QPushButton(uiText(name));
            form->addRow(button);
            auto target = &value;
            connect(button, &QPushButton::clicked, &dialog, [&, target, name] {
                auto result = QColorDialog::getColor(*target, &dialog, uiText(name),
                                                     QColorDialog::ShowAlphaChannel);
                if (result.isValid())
                    *target = result;
            });
        };
        color("Grid Color", options.gridColor);
        color("Guide Color", options.guideColor);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted) {
            options.gridSpacing = spacing->value();
            options.gridSubdivisions = subdivisions->value();
            applyViewOptions(options);
        }
    });
}
} // namespace compositor
