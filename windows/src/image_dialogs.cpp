// SPDX-License-Identifier: MIT
#include "editor.h"
#include "image_operations.h"
#include "language.h"
#include "render.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSignalBlocker>
#include <cmath>

namespace compositor {
namespace {
class Dimensions {
  public:
    QDoubleSpinBox *width, *height, *resolution;
    QComboBox *units;
    QCheckBox *linked, *relative;
    QSize original;
    Dimensions(QDialog &dialog, QFormLayout &form, const Document &document)
        : original(document.size()) {
        width = new QDoubleSpinBox(&dialog);
        height = new QDoubleSpinBox(&dialog);
        resolution = new QDoubleSpinBox(&dialog);
        units = new QComboBox(&dialog);
        linked = new QCheckBox("Constrain proportions", &dialog);
        relative = new QCheckBox("Relative", &dialog);
        width->setObjectName("sizeWidth");
        height->setObjectName("sizeHeight");
        resolution->setObjectName("sizeResolution");
        units->setObjectName("sizeUnits");
        linked->setObjectName("sizeLinked");
        relative->setObjectName("sizeRelative");
        for (auto spin : {width, height}) {
            spin->setDecimals(3);
            spin->setRange(0.001, MaxSide * 100.0);
        }
        resolution->setRange(1, 9600);
        resolution->setValue(document.metadata.value("resolution").toDouble(72));
        units->addItems({"Pixels", "Percent", "Inches", "Centimeters"});
        width->setValue(original.width());
        height->setValue(original.height());
        form.addRow("Width", width);
        form.addRow("Height", height);
        form.addRow("Units", units);
        form.addRow(linked);
        QObject::connect(width, &QDoubleSpinBox::valueChanged, &dialog, [this](double value) {
            if (linked->isChecked() && !relative->isChecked()) {
                QSignalBlocker blocker(height);
                height->setValue(units->currentIndex() == 1
                                     ? value
                                     : value * original.height() / original.width());
            }
        });
        QObject::connect(height, &QDoubleSpinBox::valueChanged, &dialog, [this](double value) {
            if (linked->isChecked() && !relative->isChecked()) {
                QSignalBlocker blocker(width);
                width->setValue(units->currentIndex() == 1
                                    ? value
                                    : value * original.width() / original.height());
            }
        });
        QObject::connect(units, &QComboBox::currentIndexChanged, &dialog, [this](int index) {
            const double x = pixels(width->value(), original.width(), previousUnit),
                         y = pixels(height->value(), original.height(), previousUnit);
            previousUnit = index;
            setValues(x, y);
        });
        QObject::connect(relative, &QCheckBox::toggled, &dialog, [this](bool checked) {
            linked->setEnabled(!checked);
            for (auto spin : {width, height})
                spin->setMinimum(checked ? -MaxSide * 100.0 : 0.001);
            setValues(checked ? 0 : original.width(), checked ? 0 : original.height());
        });
    }
    QSize size() const {
        return {int(std::round(pixels(width->value(), original.width(), units->currentIndex()))) +
                    (relative->isChecked() ? original.width() : 0),
                int(std::round(pixels(height->value(), original.height(), units->currentIndex()))) +
                    (relative->isChecked() ? original.height() : 0)};
    }
    void setValues(double x, double y) {
        QSignalBlocker bx(width), by(height);
        width->setValue(display(x, original.width()));
        height->setValue(display(y, original.height()));
    }

  private:
    int previousUnit = 0;
    double pixels(double value, double side, int unit) const {
        switch (unit) {
        case 1:
            return value * side / 100;
        case 2:
            return value * resolution->value();
        case 3:
            return value * resolution->value() / 2.54;
        default:
            return value;
        }
    }
    double display(double value, double side) const {
        switch (units->currentIndex()) {
        case 1:
            return value * 100 / side;
        case 2:
            return value / resolution->value();
        case 3:
            return value * 2.54 / resolution->value();
        default:
            return value;
        }
    }
};
void buttons(QDialog &dialog, QFormLayout &form) {
    auto box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form.addRow(box);
    QObject::connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
}
} // namespace
void EditorWindow::canvasSizeDialog() {
    auto p = page();
    if (!p)
        return;
    p->canvas->cancelInteraction();
    QDialog dialog(this);
    dialog.setWindowTitle("Canvas Size");
    dialog.setObjectName("canvasSizeDialog");
    QFormLayout form(&dialog);
    Dimensions dimensions(dialog, form, p->document);
    form.addRow(dimensions.relative);
    auto grid = new QGridLayout;
    QButtonGroup anchors(&dialog);
    const QStringList names{"Top left",     "Top center",  "Top right",     "Middle left", "Center",
                            "Middle right", "Bottom left", "Bottom center", "Bottom right"};
    for (int i = 0; i < 9; ++i) {
        auto anchor = new QRadioButton(&dialog);
        anchor->setObjectName("canvasAnchor" + QString::number(i));
        anchor->setToolTip(names[i]);
        anchor->setAccessibleName(names[i]);
        anchors.addButton(anchor, i);
        grid->addWidget(anchor, i / 3, i % 3);
        anchor->setChecked(i == 4);
    }
    form.addRow("Anchor", grid);
    QComboBox fill;
    fill.setObjectName("canvasFill");
    fill.addItems({"Transparent", "White", "Black", "Foreground color", "Background color"});
    form.addRow("Canvas extension color", &fill);
    buttons(dialog, form);
    if (dialog.exec() != QDialog::Accepted)
        return;
    std::optional<QColor> color;
    switch (fill.currentIndex()) {
    case 1:
        color = Qt::white;
        break;
    case 2:
        color = Qt::black;
        break;
    case 3:
        color = p->session.foreground;
        break;
    case 4:
        color = p->session.background;
        break;
    }
    if (dimensions.size() == p->document.size())
        return;
    p->edit("Canvas Size",
            [&](Document &d) { resizeCanvas(d, dimensions.size(), anchors.checkedId(), color); });
    p->canvas->cancelCropFrame();
    p->canvas->fit();
}
void EditorWindow::imageSizeDialog() {
    auto p = page();
    if (!p)
        return;
    p->canvas->cancelInteraction();
    QDialog dialog(this);
    dialog.setWindowTitle("Image Size");
    dialog.setObjectName("imageSizeDialog");
    QFormLayout form(&dialog);
    Dimensions dimensions(dialog, form, p->document);
    dimensions.linked->setChecked(true);
    form.addRow("Resolution (pixels/inch)", dimensions.resolution);
    QCheckBox resample("Resample");
    resample.setObjectName("imageResample");
    resample.setChecked(true);
    QComboBox sampling;
    sampling.addItems({"Nearest", "High quality"});
    sampling.setCurrentIndex(1);
    form.addRow(&resample);
    form.addRow("Sampling", &sampling);
    auto note = new QLabel("Resampling rasterizes text, shapes and rotated layers.");
    note->setWordWrap(true);
    form.addRow(note);
    connect(&resample, &QCheckBox::toggled, &dialog, [&](bool enabled) {
        dimensions.width->setEnabled(enabled);
        dimensions.height->setEnabled(enabled);
        dimensions.linked->setEnabled(enabled);
        sampling.setEnabled(enabled);
        if (!enabled)
            dimensions.setValues(p->document.size().width(), p->document.size().height());
    });
    connect(dimensions.resolution, &QDoubleSpinBox::valueChanged, &dialog, [&] {
        if (!resample.isChecked())
            dimensions.setValues(p->document.size().width(), p->document.size().height());
    });
    buttons(dialog, form);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QSize size = resample.isChecked() ? dimensions.size() : p->document.size();
    const double dpi = dimensions.resolution->value();
    if (size == p->document.size() && dpi == p->document.metadata.value("resolution").toDouble(72))
        return;
    p->edit("Image Size", [&](Document &d) {
        resizeImage(d, size, dpi,
                    sampling.currentIndex() == 0 ? Qt::FastTransformation
                                                 : Qt::SmoothTransformation);
    });
    p->canvas->cancelCropFrame();
    p->canvas->fit();
}
void EditorWindow::trimDialog() {
    auto p = page();
    if (!p)
        return;
    require(p->document.previewLimitations().isEmpty(),
            "Unsupported effects must be migrated before trimming");
    QDialog dialog(this);
    dialog.setWindowTitle("Trim");
    QFormLayout form(&dialog);
    QComboBox basis;
    basis.addItems({"Transparent pixels", "Top-left pixel color", "Bottom-right pixel color"});
    form.addRow("Based on", &basis);
    QCheckBox top("Top"), bottom("Bottom"), left("Left"), right("Right");
    for (auto side : {&top, &bottom, &left, &right}) {
        side->setChecked(true);
        form.addRow(side);
    }
    QDoubleSpinBox tolerance;
    tolerance.setRange(0, 255);
    tolerance.setDecimals(0);
    tolerance.setEnabled(false);
    form.addRow("Tolerance", &tolerance);
    connect(&basis, &QComboBox::currentIndexChanged, &dialog,
            [&](int i) { tolerance.setEnabled(i != 0); });
    buttons(dialog, form);
    if (dialog.exec() != QDialog::Accepted)
        return;
    TrimOptions options;
    options.basis = TrimOptions::Basis(basis.currentIndex());
    options.top = top.isChecked();
    options.bottom = bottom.isChecked();
    options.left = left.isChecked();
    options.right = right.isChecked();
    options.tolerance = int(tolerance.value());
    const auto bounds = trimBounds(renderDocument(p->document), options);
    require(!bounds.isEmpty(), "No content remains after trimming");
    if (bounds == QRect(QPoint(), p->document.size()))
        return;
    p->edit("Trim", [&](Document &d) { cropCanvas(d, bounds); });
    p->canvas->cancelCropFrame();
    p->canvas->fit();
}
void EditorWindow::flipDocument(bool horizontal) {
    auto p = page();
    if (!p)
        return;
    p->edit(horizontal ? "Flip Canvas Horizontal" : "Flip Canvas Vertical", [&](Document &d) {
        flipCanvas(d, horizontal);
        if (!p->session.selection.isNull())
            p->session.selection =
                p->session.selection.flipped(horizontal ? Qt::Horizontal : Qt::Vertical);
    });
    p->canvas->cancelCropFrame();
}
void EditorWindow::applyCrop(QRect bounds) {
    auto p = page();
    if (!p || bounds == QRect(QPoint(), p->document.size()))
        return;
    p->edit("Crop Canvas", [&](Document &d) { cropCanvas(d, bounds); });
    p->canvas->fit();
}
} // namespace compositor
