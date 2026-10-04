// SPDX-License-Identifier: MIT
#include "ai_background.h"
#include "ai_models_dialog.h"
#include "editor.h"
#include "language.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
namespace compositor {
namespace {
class BackgroundPreview final : public QWidget {
  public:
    QImage image;
    explicit BackgroundPreview(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(540, 340);
        setObjectName("aiBackgroundPreview");
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(45, 45, 45));
        if (image.isNull())
            return;
        auto size = QSizeF(image.size());
        size.scale(QSizeF(this->size()), Qt::KeepAspectRatio);
        const QRectF area(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2),
                          size);
        painter.save();
        painter.setClipRect(area);
        constexpr int step = 12;
        for (int y = 0; y < height(); y += step)
            for (int x = 0; x < width(); x += step)
                painter.fillRect(x, y, step, step,
                                 ((x / step + y / step) % 2) ? QColor(185, 185, 185)
                                                             : QColor(225, 225, 225));
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(area, image);
        painter.restore();
    }
};
struct BackgroundJob {
    QImage raw, mask, preview;
    QString backend, error;
    bool canceled = false;
};
} // namespace
void EditorWindow::removeBackground() {
    QPointer<EditorPage> p = page();
    if (!p)
        return;
    p->canvas->cancelInteraction();
    const auto active = p->document.active();
    require(active && !active->image.isNull() && !active->group(), "Select a pixel layer");
    const auto source = *active;
    const auto snapshot = p->document;
    const auto selection = p->session.selection;
    const auto revision = p->revision;
    const auto service = p->aiSelection;
    QDialog dialog(this);
    dialog.setObjectName("aiBackgroundDialog");
    dialog.setWindowTitle("Remove Background");
    auto layout = new QVBoxLayout(&dialog);
    auto description =
        new QLabel("Hide the background with a layer mask. Original pixels are preserved.");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto quality = new QComboBox;
    quality->setObjectName("aiBackgroundQuality");
    quality->addItems({"Basic", "Advanced"});
    auto form = new QFormLayout;
    form->addRow("Quality", quality);
    auto refine = new QSpinBox;
    refine->setObjectName("aiBackgroundRefine");
    refine->setRange(0, 40);
    refine->setValue(12);
    refine->setSuffix(" px");
    form->addRow("Refine", refine);
    auto contrast = new QSpinBox;
    contrast->setObjectName("aiBackgroundContrast");
    contrast->setRange(0, 100);
    contrast->setValue(25);
    contrast->setSuffix(" %");
    form->addRow("Contrast", contrast);
    auto shift = new QSpinBox;
    shift->setObjectName("aiBackgroundShift");
    shift->setRange(-10, 10);
    shift->setSuffix(" px");
    form->addRow("Shift Edge", shift);
    layout->addLayout(form);
    auto preview = new BackgroundPreview;
    layout->addWidget(preview, 1);
    auto showMask = new QCheckBox("Show Mask");
    showMask->setObjectName("aiBackgroundShowMask");
    layout->addWidget(showMask);
    auto status = new QLabel("Calculating background mask…");
    status->setObjectName("aiBackgroundStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto models = new QPushButton("Manage AI Models…");
    layout->addWidget(models);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    auto ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(false);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(p, &EditorPage::documentChanged, &dialog, &QDialog::reject);
    connect(p, &QObject::destroyed, &dialog, &QDialog::reject);
    QFutureWatcher<BackgroundJob> watcher;
    QTimer debounce;
    debounce.setSingleShot(true);
    debounce.setInterval(60);
    QImage raw, currentMask, currentPreview, committedMask;
    QString backend;
    std::shared_ptr<AiCancellation> cancellation;
    quint64 generation = 0, runningGeneration = 0;
    bool running = false, inferring = false, committing = false;
    auto display = [&] {
        preview->image = showMask->isChecked() ? currentMask : currentPreview;
        preview->update();
    };
    auto settings = [&] {
        return BackgroundSettings{quality->currentIndex() == 1, refine->value(), contrast->value(),
                                  shift->value()};
    };
    auto start = [&](bool full) {
        if (running || !p)
            return;
        running = true;
        inferring = raw.isNull();
        runningGeneration = generation;
        cancellation = std::make_shared<AiCancellation>();
        const auto cancel = cancellation;
        const auto cached = raw;
        const auto options = settings();
        const auto previousBackend = backend;
        watcher.setFuture(QtConcurrent::run([source, snapshot, selection, service, cached, cancel,
                                             options, full, previousBackend] {
            BackgroundJob job;
            try {
                job.raw = cached;
                job.backend = previousBackend;
                if (job.raw.isNull()) {
                    auto inferred = service->matte(source.image, cancel);
                    job.raw = inferred.mask;
                    job.backend = inferred.backend;
                }
                require(job.raw.size() == source.image.size() &&
                            job.raw.format() == QImage::Format_Grayscale8,
                        "Invalid background matte");
                bool found = false;
                for (int y = 0; y < job.raw.height() && !found; ++y) {
                    cancel->check();
                    const auto row = job.raw.constScanLine(y);
                    found = std::any_of(row, row + job.raw.width(),
                                        [](uchar value) { return value > 127; });
                }
                require(found, "No foreground subject was detected in this layer. Try an image "
                               "with a more distinct subject.");
                auto layer = source;
                auto matte = job.raw;
                auto scaled = options;
                if (!full && std::max(layer.image.width(), layer.image.height()) > 1400) {
                    const auto size = layer.image.size().scaled(1400, 1400, Qt::KeepAspectRatio);
                    const double factor = double(size.width()) / layer.image.width();
                    layer.image =
                        layer.image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    matte = matte.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    scaled.refine =
                        options.refine ? std::max(1, int(std::lround(options.refine * factor))) : 0;
                    scaled.shift =
                        options.shift
                            ? int(std::copysign(
                                  std::max(1, int(std::lround(std::abs(options.shift) * factor))),
                                  options.shift))
                            : 0;
                }
                job.mask = backgroundLayerMask(
                    snapshot, layer, refineBackgroundMatte(matte, layer.image, scaled, cancel),
                    selection, cancel);
                if (!full)
                    job.preview = backgroundCutout(layer.image, job.mask, cancel);
            } catch (const AiCancelled &) {
                job.canceled = true;
            } catch (const std::exception &e) {
                job.error = QString::fromUtf8(e.what());
            }
            return job;
        }));
    };
    auto request = [&] {
        if (committing)
            return;
        ++generation;
        ok->setEnabled(false);
        refine->setEnabled(quality->currentIndex() == 1);
        contrast->setEnabled(quality->currentIndex() == 1);
        shift->setEnabled(quality->currentIndex() == 1);
        if (running && !inferring && cancellation)
            cancellation->cancel();
        status->setText(uiText("Calculating background mask…"));
        debounce.start();
    };
    connect(&debounce, &QTimer::timeout, &dialog, [&] { start(false); });
    connect(&watcher, &QFutureWatcherBase::finished, &dialog, [&] {
        running = false;
        const auto result = watcher.result();
        if (!result.raw.isNull() && result.error.isEmpty()) {
            raw = result.raw;
            backend = result.backend;
        }
        if (runningGeneration != generation) {
            debounce.start();
            return;
        }
        if (result.canceled || !result.error.isEmpty()) {
            committing = false;
            quality->setEnabled(true);
            models->setEnabled(true);
            refine->setEnabled(quality->currentIndex() == 1);
            contrast->setEnabled(quality->currentIndex() == 1);
            shift->setEnabled(quality->currentIndex() == 1);
            currentMask = {};
            currentPreview = {};
            display();
            status->setText(result.canceled ? uiText("Selection canceled.") : uiText(result.error));
            return;
        }
        if (committing) {
            committedMask = result.mask;
            dialog.accept();
            return;
        }
        currentMask = result.mask;
        currentPreview = result.preview;
        display();
        ok->setEnabled(true);
        status->setText(uiText("Background preview ready.") + " " + backend);
    });
    connect(quality, &QComboBox::currentIndexChanged, &dialog, request);
    for (auto control : {refine, contrast, shift})
        connect(control, &QSpinBox::valueChanged, &dialog, request);
    connect(showMask, &QCheckBox::toggled, &dialog, display);
    connect(models, &QPushButton::clicked, &dialog, [&] {
        AiModelsDialog manager(&dialog);
        manager.exec();
        request();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (running || !ok->isEnabled())
            return;
        committing = true;
        debounce.stop();
        ok->setEnabled(false);
        quality->setEnabled(false);
        models->setEnabled(false);
        for (auto control : {refine, contrast, shift})
            control->setEnabled(false);
        status->setText(uiText("Applying full-resolution background mask…"));
        start(true);
    });
    request();
    dialog.resize(620, 650);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    debounce.stop();
    if (cancellation)
        cancellation->cancel();
    disconnect(&watcher, nullptr, &dialog, nullptr);
    if (!accepted || committedMask.isNull() || !p || p->revision != revision ||
        p->session.selection != selection || p->document.activeId() != source.id())
        return;
    const auto current = p->document.find(source.id());
    if (!current || current->image.cacheKey() != source.image.cacheKey() ||
        current->transform() != source.transform())
        return;
    p->edit("Remove Background", [&](Document &document) {
        installBackgroundMask(*document.find(source.id()), committedMask);
    });
    p->session.target = EditTarget::Mask;
    emit p->canvas->sessionChanged();
}
} // namespace compositor
