// SPDX-License-Identifier: MIT
#include "editor.h"
#include "language.h"
#include "selection_operations.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QInputDialog>
#include <QProgressDialog>
#include <QMessageBox>
#include <memory>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <cmath>
#include <functional>

namespace compositor {
void EditorWindow::maskFromSelection() {
    auto p = page();
    if (!p || p->session.selection.isNull())
        return;
    p->canvas->cancelInteraction();
    const auto mask = p->session.selection;
    p->edit("Mask from Selection", [mask](Document &d) {
        auto layer = d.active();
        require(layer, "Select a layer");
        layer->mask = mask;
        layer->metadata["maskFile"] = layer->id() + ".mask.png";
        layer->metadata["maskPlacement"] = QJsonObject{
            {"origin", QJsonArray{0, 0}}, {"size", QJsonArray{d.size().width(), d.size().height()}},
            {"rotation", 0}, {"flipX", false}, {"flipY", false}};
        layer->metadata["maskEnabled"] = true;
        layer->metadata["maskLinked"] = true;
    });
}
namespace {
class SamplePreview final : public QWidget {
  public:
    QImage image;
    std::function<void(QPoint, Qt::KeyboardModifiers)> sample;
    explicit SamplePreview(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(292, 200);
        setCursor(Qt::CrossCursor);
    }
    QRectF imageRect() const {
        auto size = QSizeF(image.size());
        size.scale(QSizeF(this->size()), Qt::KeepAspectRatio);
        return QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(40, 40, 40));
        if (!image.isNull())
            p.drawImage(imageRect(), image);
    }
    void mousePressEvent(QMouseEvent *event) override {
        const auto frame = imageRect();
        if (event->button() != Qt::LeftButton || image.isNull() ||
            !frame.contains(event->position()) || !sample)
            return;
        const auto position = event->position() - frame.topLeft();
        sample(QPoint(int(std::floor(position.x() * image.width() / frame.width())),
                      int(std::floor(position.y() * image.height() / frame.height()))),
               event->modifiers());
    }
};
struct RangeResult {
    QImage mask;
    QString error;
};
}
void EditorWindow::resizeSelectionDialog(bool expand) {
    auto p = page();
    if (!p || p->session.selection.isNull())
        return;
    p->canvas->cancelInteraction();
    const QString label = expand ? "Expand Selection" : "Contract Selection";
    bool accepted = false;
    const int radius = QInputDialog::getInt(this, label, "Radius (pixels)", 1, 1, 200, 1, &accepted);
    if (!accepted)
        return;
    const auto original = p->session.selection;
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    QProgressDialog progress("Updating selection…", "Cancel", 0, 0, this);
    progress.setObjectName("resizeSelectionProgress");
    progress.setWindowTitle(label);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    QFutureWatcher<RangeResult> watcher;
    connect(&progress, &QProgressDialog::canceled, &progress,
            [canceled] { canceled->store(true, std::memory_order_relaxed); });
    connect(&watcher, &QFutureWatcherBase::finished, &progress, &QDialog::accept);
    watcher.setFuture(QtConcurrent::run([original, radius, expand, canceled] {
        RangeResult result;
        try {
            result.mask = resizeSelectionMask(original, radius, expand, canceled.get());
        } catch (const std::exception &e) {
            result.error = QString::fromUtf8(e.what());
        }
        return result;
    }));
    const auto finished = progress.exec() == QDialog::Accepted;
    disconnect(&watcher, nullptr, &progress, nullptr);
    if (!finished || canceled->load()) {
        canceled->store(true, std::memory_order_relaxed);
        return;
    }
    const auto result = watcher.result();
    if (!result.error.isEmpty())
        QMessageBox::warning(this, uiText(label), result.error);
    else if (!result.mask.isNull())
        p->canvas->replaceSelection(result.mask, label);
}
void EditorWindow::colorRangeDialog() {
    auto p = page();
    if (!p)
        return;
    p->canvas->cancelInteraction();
    const auto original = p->session.selection;
    const auto image = p->canvas->fullComposite();
    QDialog dialog(this);
    dialog.setObjectName("colorRangeDialog");
    dialog.setWindowTitle("Color Range");
    dialog.resize(660, 430);
    auto layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Click the image to sample. Shift adds colors; Alt excludes colors."));
    auto previews = new QHBoxLayout;
    auto source = new SamplePreview;
    source->setObjectName("colorRangeSource");
    source->image = image;
    auto selection = new SamplePreview;
    selection->setObjectName("colorRangePreview");
    selection->setCursor(Qt::ArrowCursor);
    selection->image = QImage(image.size(), QImage::Format_Grayscale8);
    require(!selection->image.isNull(), "Not enough memory for selection");
    selection->image.fill(0);
    for (const auto &entry : {std::pair<QString, SamplePreview *>("Image", source),
                              std::pair<QString, SamplePreview *>("Selection", selection)}) {
        auto column = new QVBoxLayout;
        column->addWidget(new QLabel(entry.first));
        column->addWidget(entry.second);
        previews->addLayout(column);
    }
    layout->addLayout(previews);
    auto controls = new QHBoxLayout;
    controls->addWidget(new QLabel("Sample mode"));
    auto mode = new QComboBox;
    mode->setObjectName("colorRangeSampleMode");
    mode->addItems({"Replace", "Add", "Subtract"});
    controls->addWidget(mode);
    controls->addWidget(new QLabel("Fuzziness"));
    auto fuzziness = new QSpinBox;
    fuzziness->setObjectName("colorRangeFuzziness");
    fuzziness->setRange(0, 200);
    fuzziness->setValue(40);
    controls->addWidget(fuzziness);
    auto invert = new QCheckBox("Invert");
    invert->setObjectName("colorRangeInvert");
    controls->addWidget(invert);
    auto clear = new QPushButton("Clear Samples");
    clear->setObjectName("colorRangeClear");
    controls->addWidget(clear);
    layout->addLayout(controls);
    auto status = new QLabel("Pick a color to preview the selection.");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    auto ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QVector<QColor> include, exclude;
    QImage current;
    quint64 generation = 0, runningGeneration = 0, appliedGeneration = 0;
    bool running = false;
    QFutureWatcher<RangeResult> watcher;
    QTimer debounce;
    debounce.setSingleShot(true);
    debounce.setInterval(30);
    auto request = [&] {
        ++generation;
        ok->setEnabled(false);
        if (include.isEmpty()) {
            current = {};
            p->session.selection = original;
            selection->image.fill(0);
            selection->update();
            emit p->canvas->selectionChanged();
            p->canvas->update();
            status->setText(uiText("Pick a color to preview the selection."));
        } else {
            status->setText(uiText("Updating selection…"));
            debounce.start();
        }
    };
    std::function<void()> start;
    start = [&] {
        if (running || include.isEmpty())
            return;
        running = true;
        runningGeneration = generation;
        const auto colors = include, removed = exclude;
        const auto tolerance = fuzziness->value();
        const bool inverse = invert->isChecked();
        watcher.setFuture(QtConcurrent::run([image, colors, removed, tolerance, inverse] {
            RangeResult result;
            try {
                result.mask = colorRangeMask(image, colors, removed, tolerance, inverse);
            } catch (const std::exception &e) {
                result.error = QString::fromUtf8(e.what());
            }
            return result;
        }));
    };
    connect(&debounce, &QTimer::timeout, &dialog, start);
    connect(&watcher, &QFutureWatcherBase::finished, &dialog, [&] {
        running = false;
        if (runningGeneration != generation) {
            if (!include.isEmpty())
                debounce.start();
            return;
        }
        const auto result = watcher.result();
        if (!result.error.isEmpty()) {
            status->setText(result.error);
            return;
        }
        appliedGeneration = generation;
        current = result.mask;
        selection->image = current.scaled(584, 400, Qt::KeepAspectRatio, Qt::FastTransformation);
        selection->update();
        p->session.selection = current;
        emit p->canvas->selectionChanged();
        p->canvas->update();
        status->setText(uiText("Selection preview ready."));
        ok->setEnabled(true);
    });
    source->sample = [&](QPoint pixel, Qt::KeyboardModifiers mods) {
        if (!image.valid(pixel))
            return;
        double red = 0, green = 0, blue = 0, alpha = 0;
        for (int y = pixel.y() - 1; y <= pixel.y() + 1; ++y)
            for (int x = pixel.x() - 1; x <= pixel.x() + 1; ++x) {
                if (!image.valid(x, y))
                    continue;
                const auto color = image.pixelColor(x, y);
                red += color.redF() * color.alphaF();
                green += color.greenF() * color.alphaF();
                blue += color.blueF() * color.alphaF();
                alpha += color.alphaF();
            }
        if (alpha <= 0)
            return;
        const auto color = QColor::fromRgbF(red / alpha, green / alpha, blue / alpha);
        const int sampling = mods & Qt::AltModifier ? 2 : mods & Qt::ShiftModifier ? 1
                                                                               : mode->currentIndex();
        if (sampling == 0) {
            include.clear();
            exclude.clear();
        }
        auto &colors = sampling == 2 ? exclude : include;
        if (!colors.contains(color))
            colors.append(color);
        request();
    };
    connect(fuzziness, &QSpinBox::valueChanged, &dialog, request);
    connect(invert, &QCheckBox::toggled, &dialog, request);
    connect(clear, &QPushButton::clicked, &dialog, [&] {
        include.clear();
        exclude.clear();
        request();
    });
    const auto accepted = dialog.exec() == QDialog::Accepted;
    debounce.stop();
    disconnect(&watcher, nullptr, &dialog, nullptr);
    // The worker holds only value copies; closing the dialog never waits for it.
    p->session.selection = original;
    emit p->canvas->selectionChanged();
    p->canvas->update();
    if (accepted && appliedGeneration == generation && !include.isEmpty() && !current.isNull())
        p->canvas->replaceSelection(current, "Color Range");
}
} // namespace compositor
