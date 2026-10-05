// SPDX-License-Identifier: MIT
#include "editor.h"
#include "image_operations.h"
#include "language.h"
#include "parameter_control.h"
#include "preview_runner.h"
#include "render.h"
#include <QColorDialog>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>
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
    p->canvas->finishTextEditing(true);
    require(p->document.previewLimitations().isEmpty(),
            "Export is unavailable because this project contains features not yet rendered: " +
                p->document.previewLimitations().join(", "));
    auto path = QFileDialog::getSaveFileName(this, "Export Image", "Export.png",
                                             "PNG Image (*.png);;JPEG Image (*.jpg)");
    if (!path.isEmpty())
        exportTo(path);
}
namespace {
// The JPEG export dialog: the encoded result itself, fitted or at 100%, its file size, the quality
// and the color that fills transparency. Encoding runs in the background as the settings change;
// what is exported is exactly what was previewed.
class JpegExportDialog final : public QDialog {
  public:
    JpegExportDialog(const QImage &image, QWidget *parent) : QDialog(parent), image_(image) {
        setObjectName("jpegExportDialog");
        setWindowTitle("Export JPEG");
        auto layout = new QVBoxLayout(this);
        auto zoom = new QHBoxLayout;
        auto fit = new QPushButton("Fit");
        fit->setObjectName("jpegFit");
        auto actual = new QPushButton("100%");
        actual->setObjectName("jpegActualSize");
        zoom->addStretch();
        zoom->addWidget(fit);
        zoom->addWidget(actual);
        layout->addLayout(zoom);
        scroll_ = new QScrollArea;
        scroll_->setMinimumSize(560, 330);
        scroll_->setAlignment(Qt::AlignCenter);
        preview_ = new QLabel;
        preview_->setObjectName("jpegPreview");
        preview_->setAlignment(Qt::AlignCenter);
        scroll_->setWidget(preview_);
        layout->addWidget(scroll_, 1);
        auto form = new QFormLayout;
        quality_ = new ParameterControl(1, 100, 0, QSettings().value("export/jpegQuality", 90).toInt(), 90);
        addParameter(form, "Quality", quality_, "jpegQualityControl");
        matteButton_ = new QPushButton;
        matteButton_->setObjectName("jpegMatte");
        form->addRow("Background for transparency", matteButton_);
        layout->addLayout(form);
        auto info = new QHBoxLayout;
        info->addWidget(new QLabel(QString("%1 × %2 px · sRGB").arg(image.width()).arg(image.height())));
        info->addStretch();
        size_ = new QLabel("Updating…");
        size_->setObjectName("jpegSize");
        info->addWidget(size_);
        layout->addLayout(info);
        buttons_ = new QDialogButtonBox(QDialogButtonBox::Cancel);
        exportButton_ = buttons_->addButton("Export", QDialogButtonBox::AcceptRole);
        exportButton_->setObjectName("jpegExport");
        layout->addWidget(buttons_);
        connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(fit, &QPushButton::clicked, this, [this] { setActualSize(false); });
        connect(actual, &QPushButton::clicked, this, [this] { setActualSize(true); });
        connect(quality_, &ParameterControl::valueChanged, this, [this] { encode(); });
        connect(matteButton_, &QPushButton::clicked, this, [this] {
            const auto picked = QColorDialog::getColor(matte_, this, uiText("Background for transparency"));
            if (picked.isValid())
                setMatte(picked);
        });
        connect(&runner_, &PreviewRunner::ready, this, [this](const QImage &decoded) {
            decoded_ = decoded;
            data_ = decoded.text("jpegData").toLatin1();
            data_ = QByteArray::fromBase64(data_);
            readyQuality_ = decoded.text("quality").toInt();
            readyMatte_ = QColor(decoded.text("matte"));
            showPreview();
        });
        connect(&runner_, &PreviewRunner::failed, this, [this](const QString &message) {
            size_->setText(uiText(message));
        });
        setMatte(Qt::white);
    }
    int quality() const {
        return int(quality_->value());
    }
    QColor matte() const {
        return matte_;
    }
    void setMatte(const QColor &color) {
        matte_ = color;
        matteButton_->setStyleSheet(QString("background:%1;min-width:48px").arg(color.name()));
        encode();
    }
    // The bytes to write: the previewed encoding when it matches the settings, else encoded now.
    QByteArray data() const {
        if (ready())
            return data_;
        return encodeJpeg(image_, quality(), matte_);
    }
    bool ready() const {
        return !data_.isEmpty() && readyQuality_ == quality() && readyMatte_ == matte_;
    }
    void accept() override {
        QSettings().setValue("export/jpegQuality", quality());
        QDialog::accept();
    }

  private:
    QImage image_, decoded_;
    QByteArray data_;
    int readyQuality_ = -1;
    QColor matte_, readyMatte_;
    bool actualSize_ = false;
    PreviewRunner runner_;
    QScrollArea *scroll_;
    QLabel *preview_, *size_;
    ParameterControl *quality_;
    QPushButton *matteButton_, *exportButton_;
    QDialogButtonBox *buttons_;
    void encode() {
        size_->setText(uiText("Updating…"));
        exportButton_->setEnabled(false);
        runner_.request([image = image_, quality = quality(), matte = matte_]() -> QImage {
            const auto bytes = encodeJpeg(image, quality, matte);
            auto decoded = QImage::fromData(bytes, "JPEG");
            require(!decoded.isNull(), "Cannot encode exported image");
            decoded.setText("jpegData", QString::fromLatin1(bytes.toBase64()));
            decoded.setText("quality", QString::number(quality));
            decoded.setText("matte", matte.name());
            return decoded;
        });
    }
    void showPreview() {
        if (decoded_.isNull())
            return;
        const bool current = ready();
        exportButton_->setEnabled(current);
        if (current)
            size_->setText(data_.size() >= 1024 * 1024
                               ? QString("%1 MB").arg(data_.size() / 1048576.0, 0, 'f', 1)
                               : QString("%1 KB").arg((data_.size() + 1023) / 1024));
        const auto shown = actualSize_ ? decoded_
                                       : decoded_.scaled(scroll_->viewport()->size().boundedTo(decoded_.size()),
                                                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
        preview_->setPixmap(QPixmap::fromImage(shown));
        preview_->resize(shown.size());
    }
    void setActualSize(bool actual) {
        actualSize_ = actual;
        showPreview();
    }
};
} // namespace
void EditorWindow::exportTo(const QString &path) {
    auto p = page();
    if (!p)
        return;
    p->canvas->finishTextEditing(true);
    p->canvas->commitFloatingSelection();
    require(p->document.previewLimitations().isEmpty(),
            "Export is unavailable because this project contains features not yet rendered: " +
                p->document.previewLimitations().join(", "));
    auto image = renderDocument(p->document);
    image.setDotsPerMeterX(int(p->document.metadata.value("resolution").toDouble(72) / 0.0254));
    image.setDotsPerMeterY(image.dotsPerMeterX());
    QByteArray bytes;
    if (path.endsWith(".jpg", Qt::CaseInsensitive) || path.endsWith(".jpeg", Qt::CaseInsensitive)) {
        JpegExportDialog dialog(image, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        bytes = dialog.data();
    }
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot open export destination");
    if (bytes.isEmpty()) {
        QImageWriter writer(&file, "PNG");
        require(writer.write(image), "Cannot encode exported image");
    } else
        require(file.write(bytes) == bytes.size(), "Cannot encode exported image");
    require(file.commit(), "Cannot encode exported image");
    statusBar()->showMessage(uiText("Exported " + path), 5000);
}
} // namespace compositor
