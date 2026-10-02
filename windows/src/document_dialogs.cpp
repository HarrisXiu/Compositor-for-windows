// SPDX-License-Identifier: MIT
#include "editor.h"
#include "language.h"
#include "render.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImageWriter>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QSaveFile>
#include <QStatusBar>

namespace compositor {
void EditorWindow::createDocument() {
    QDialog dialog(this);
    dialog.setWindowTitle("New Canvas");
    auto layout = new QFormLayout(&dialog);
    QDoubleSpinBox w, h;
    w.setRange(1, MaxSide);
    h.setRange(1, MaxSide);
    w.setValue(1200);
    h.setValue(800);
    w.setDecimals(0);
    h.setDecimals(0);
    layout->addRow("Width", &w);
    layout->addRow("Height", &h);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        addPage(Document::create({int(w.value()), int(h.value())}));
}
void EditorWindow::exportImage() {
    auto p = page();
    if (!p)
        return;
    require(p->document.previewLimitations().isEmpty(),
            "Export is unavailable because this project contains features not yet rendered: " +
                p->document.previewLimitations().join(", "));
    auto path = QFileDialog::getSaveFileName(this, "Export Image", "Export.png",
                                             "PNG Image (*.png);;JPEG Image (*.jpg)");
    if (path.isEmpty())
        return;
    auto image = renderDocument(p->document);
    image.setDotsPerMeterX(int(p->document.metadata.value("resolution").toDouble(72) / 0.0254));
    image.setDotsPerMeterY(image.dotsPerMeterX());
    QByteArray format = "PNG";
    int quality = 95;
    if (path.endsWith(".jpg", Qt::CaseInsensitive) || path.endsWith(".jpeg", Qt::CaseInsensitive)) {
        format = "JPEG";
        bool ok;
        quality = QInputDialog::getInt(this, "JPEG Quality", "Quality", 95, 1, 100, 1, &ok);
        if (!ok)
            return;
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter painter(&flat);
        painter.drawImage(0, 0, image);
        painter.end();
        flat.setDotsPerMeterX(image.dotsPerMeterX());
        flat.setDotsPerMeterY(image.dotsPerMeterY());
        image = flat;
    }
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot open export destination");
    QImageWriter writer(&file, format);
    writer.setQuality(quality);
    require(writer.write(image) && file.commit(), "Cannot encode exported image");
    statusBar()->showMessage(uiText("Exported " + path), 5000);
}
} // namespace compositor
