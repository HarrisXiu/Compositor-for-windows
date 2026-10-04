#include "ai_models_dialog.h"
#include "language.h"
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace compositor {
AiModelsDialog::AiModelsDialog(QWidget *parent, QString root)
    : QDialog(parent), root_(root.isEmpty() ? aiModelCacheRoot() : std::move(root)),
      downloader_(new AiModelDownloader(root_, this)), checking_(std::make_shared<AiCancellation>()) {
    setObjectName("aiModelsDialog");
    setWindowTitle("AI Models");
    resize(620, 480);
    auto layout = new QVBoxLayout(this);
    auto explanation = new QLabel("Models run locally. Downloads are optional and SHA256 verified; installed models remain available offline.");
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto publication = new QLabel("The model release has not been published yet. Import matching local files; unpublished download URLs are not requested.");
    publication->setWordWrap(true);
    layout->addWidget(publication);
    auto cache = new QLabel(root_);
    cache->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(cache);
    const auto &catalog = aiModelCatalog();
    for (int index = 0; index < catalog.size(); ++index) {
        const auto &model = catalog[index];
        auto group = new QGroupBox(model.name);
        auto body = new QVBoxLayout(group);
        qint64 bytes = 0;
        for (const auto &file : model.assets) bytes += file.size;
        body->addWidget(new QLabel(QString::number(bytes / 1000000.0, 'f', 1) + " MB — " + model.license));
        Row row;
        row.status = new QLabel("Checking model cache…");
        row.status->setWordWrap(true);
        row.progress = new QProgressBar;
        row.progress->setRange(0, 100);
        row.progress->setValue(0);
        auto buttons = new QHBoxLayout;
        row.download = new QPushButton("Download / Resume");
        row.restart = new QPushButton("Restart Download");
        row.import = new QPushButton("Import Local Files…");
        row.download->setObjectName("download_" + model.id);
        row.import->setObjectName("import_" + model.id);
        row.download->setEnabled(model.published);
        row.restart->setEnabled(model.published);
        buttons->addWidget(row.download);
        buttons->addWidget(row.restart);
        buttons->addWidget(row.import);
        body->addWidget(row.status);
        body->addWidget(row.progress);
        body->addLayout(buttons);
        layout->addWidget(group);
        rows_.push_back(row);
        connect(row.import, &QPushButton::clicked, this, [this, index] { importModel(index); });
        connect(row.download, &QPushButton::clicked, this, [this, index] { downloadModel(index, false); });
        connect(row.restart, &QPushButton::clicked, this, [this, index] {
            if (QMessageBox::question(this, uiText("Restart Download"), uiText("Discard this model's partial download and start again?")) == QMessageBox::Yes)
                downloadModel(index, true);
        });
    }
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    cancel_ = buttons->addButton("Cancel Transfer", QDialogButtonBox::ActionRole);
    cancel_->setEnabled(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (operation_) operation_->cancel();
        downloader_->cancel();
    });
    layout->addWidget(buttons);
    connect(downloader_, &AiModelDownloader::progress, this, [this](qint64 received, qint64 total) {
        if (current_ < 0) return;
        rows_[current_].progress->setRange(0, 100);
        rows_[current_].progress->setValue(total ? int(double(received) / total * 100) : 0);
        rows_[current_].status->setText(uiText("Downloading and verifying model component") + " " + QString::number(asset_ + 1));
    });
    connect(downloader_, &AiModelDownloader::finished, this, [this] { ++asset_; nextAsset(); });
    connect(downloader_, &AiModelDownloader::failed, this, [this](const QString &error) { endOperation(error); });
    connect(downloader_, &AiModelDownloader::cancelled, this, [this] { endOperation(uiText("Cancelled. Partial downloads are preserved for Resume.")); });
    UiLanguage::instance().translateObject(this, true);
    refresh();
}
AiModelsDialog::~AiModelsDialog() {
    checking_->cancel();
    if (operation_) operation_->cancel();
    downloader_->cancel();
}
void AiModelsDialog::reject() {
    checking_->cancel();
    if (operation_) operation_->cancel();
    downloader_->cancel();
    QDialog::reject();
}
void AiModelsDialog::enableActions(bool enabled) {
    for (int index = 0; index < rows_.size(); ++index) {
        rows_[index].download->setEnabled(enabled && aiModelCatalog()[index].published);
        rows_[index].restart->setEnabled(enabled && aiModelCatalog()[index].published);
        rows_[index].import->setEnabled(enabled);
    }
    cancel_->setEnabled(!enabled);
}
void AiModelsDialog::refresh(int preserveStatus) {
    checking_ = std::make_shared<AiCancellation>();
    for (int index = 0; index < rows_.size(); ++index) {
        if (index == preserveStatus) continue;
        auto watcher = new QFutureWatcher<AiModelState>(this);
        connect(watcher, &QFutureWatcher<AiModelState>::finished, this, [this, watcher, index, token = checking_] {
            if (current_ < 0 && !token->cancelled()) {
                try {
                    const auto state = watcher->result();
                    rows_[index].status->setText(uiText(state.message));
                    rows_[index].progress->setValue(state.installed ? 100 : 0);
                } catch (const std::exception &error) {
                    rows_[index].status->setText(QString::fromUtf8(error.what()));
                }
            }
            watcher->deleteLater();
        });
        watcher->setFuture(inspectAiModelAsync(aiModelCatalog()[index], root_, checking_));
    }
}
void AiModelsDialog::importModel(int index) {
    const auto source = QFileDialog::getExistingDirectory(this, uiText("Import Local Files…"));
    if (source.isEmpty()) return;
    checking_->cancel();
    current_ = index;
    operation_ = std::make_shared<AiCancellation>();
    enableActions(false);
    rows_[index].progress->setRange(0, 0);
    rows_[index].status->setText(uiText("Importing and verifying local model files…"));
    auto watcher = new QFutureWatcher<AiModelInstallResult>(this);
    connect(watcher, &QFutureWatcher<AiModelInstallResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        endOperation(result.success ? uiText("Model installed and verified.") : result.cancelled ? uiText("Cancelled.") : result.error, result.success);
    });
    watcher->setFuture(importAiModelAsync(aiModelCatalog()[index], source, root_, operation_));
}
void AiModelsDialog::downloadModel(int index, bool restart) {
    checking_->cancel();
    current_ = index;
    asset_ = 0;
    restart_ = restart;
    operation_ = std::make_shared<AiCancellation>();
    enableActions(false);
    nextAsset();
}
void AiModelsDialog::nextAsset() {
    if (current_ < 0) return;
    if (operation_->cancelled()) { endOperation(uiText("Cancelled.")); return; }
    const auto model = aiModelCatalog()[current_];
    if (asset_ == model.assets.size()) {
        try {
            saveAiModelLicenses(model, root_);
            endOperation(uiText("Model installed and verified."), true);
        } catch (const std::exception &error) {
            endOperation(QString::fromUtf8(error.what()));
        }
        return;
    }
    auto watcher = new QFutureWatcher<bool>(this);
    const auto file = model.assets[asset_];
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, model] {
        try {
            const bool installed = watcher->result();
            watcher->deleteLater();
            if (operation_->cancelled()) { endOperation(uiText("Cancelled.")); return; }
            if (installed) { ++asset_; nextAsset(); }
            else downloader_->start(model, asset_, restart_);
        } catch (const std::exception &error) {
            watcher->deleteLater();
            endOperation(QString::fromUtf8(error.what()));
        }
    });
    watcher->setFuture(QtConcurrent::run([model, file, root = root_, cancelled = operation_] {
        return verifyAiAsset(aiModelPath(model, file, root), file, cancelled);
    }));
}
void AiModelsDialog::endOperation(const QString &message, bool installed) {
    if (current_ >= 0) {
        rows_[current_].status->setText(message);
        rows_[current_].progress->setRange(0, 100);
        rows_[current_].progress->setValue(installed ? 100 : 0);
    }
    const int completed = current_;
    current_ = -1;
    operation_.reset();
    enableActions(true);
    refresh(completed);
}
}
