#include "ai_download.h"
#include "document.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtConcurrent/QtConcurrentRun>

namespace compositor {
AiModelDownloader::AiModelDownloader(QString root, QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), root_(root.isEmpty() ? aiModelCacheRoot() : std::move(root)),
      network_(network ? network : new QNetworkAccessManager(this)) {}
AiModelDownloader::~AiModelDownloader() {
    if (cancellation_) cancellation_->cancel();
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
    }
}
bool AiModelDownloader::busy() const { return active_; }
void AiModelDownloader::start(const AiModelDefinition &model, int index, bool restart) {
    if (active_) { emit failed("Another model transfer is already active."); return; }
    try {
        require(model.published, "This model release has not been published. Import verified local model files instead.");
        require(index >= 0 && index < model.assets.size(), "Invalid model component");
        model_ = model;
        asset_ = model.assets[index];
        require(asset_.url.isValid() && asset_.url.scheme() == "https" && asset_.url.userInfo().isEmpty(), "Model downloads require an HTTPS URL without credentials");
        destination_ = aiModelPath(model, asset_, root_);
        part_ = destination_ + ".part";
        metadata_ = destination_ + ".resume.json";
        const auto parent = QFileInfo(destination_).absolutePath();
        require(QDir().mkpath(parent) && !QFileInfo(parent).isSymLink() && !QFileInfo(destination_).isSymLink() &&
                !QFileInfo(part_).isSymLink() && !QFileInfo(metadata_).isSymLink(), "Unsafe or unwritable model cache");
        installLock_ = lockAiModelInstall(model, root_);
        cancellation_ = std::make_shared<AiCancellation>();
        active_ = true;
        headers_ = cancelRequested_ = false;
        QByteArray etag;
        if (!restart && QFileInfo(part_).size() > 0) {
            QFile resume(metadata_);
            require(resume.open(QIODevice::ReadOnly) && resume.size() <= 8192, "Partial model has no valid resume metadata. Use Restart.");
            const auto saved = QJsonDocument::fromJson(resume.readAll()).object();
            require(saved.value("sha256").toString().toUtf8() == asset_.sha256.toHex() &&
                    saved.value("size").toDouble() == double(asset_.size) && saved.value("url").toString() == asset_.url.toString(),
                    "Partial model belongs to a different release. Use Restart.");
            etag = saved.value("etag").toString().toUtf8();
        }
        file_.setFileName(part_);
        require(file_.open(QIODevice::ReadWrite), "Cannot write partial model cache");
        if (restart) require(file_.resize(0), "Cannot restart partial model download");
        offset_ = file_.size();
        require(offset_ <= asset_.size && file_.seek(offset_), "Partial model exceeds its expected size. Use Restart.");
        emit progress(offset_, asset_.size);
        if (cancelRequested_) { finishCancelled(); return; }
        if (offset_ == asset_.size) {
            file_.close();
            verifyAndInstall();
            return;
        }
        QNetworkRequest request(asset_.url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setMaximumRedirectsAllowed(5);
        request.setTransferTimeout(60000);
        request.setRawHeader("Accept-Encoding", "identity");
        request.setRawHeader("User-Agent", "Compositor-Windows/0.4 AI-model-download");
        if (offset_ > 0) {
            request.setRawHeader("Range", "bytes=" + QByteArray::number(offset_) + "-");
            if (!etag.startsWith("W/") && !etag.contains('\r') && !etag.contains('\n') && etag.size() <= 1024 && !etag.isEmpty())
                request.setRawHeader("If-Range", etag);
        }
        reply_ = network_->get(request);
        reply_->setReadBufferSize(4 * 1024 * 1024);
        connect(reply_, &QNetworkReply::metaDataChanged, this, [this] { acceptResponse(); });
        connect(reply_, &QNetworkReply::readyRead, this, &AiModelDownloader::receive);
        connect(reply_, &QNetworkReply::finished, this, &AiModelDownloader::finishNetwork);
    } catch (const std::exception &error) {
        fail(QString::fromUtf8(error.what()));
    }
}
bool AiModelDownloader::acceptResponse() {
    if (!active_ || !reply_) return false;
    if (headers_) return true;
    const auto value = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!value.isValid()) return false;
    const int status = value.toInt();
    if (status >= 300 && status < 400) return false;
    if (status != 200 && status != 206) {
        fail("Model server returned HTTP " + QString::number(status) + ". Check your connection or try again later.");
        return false;
    }
    if (status == 200 && offset_ > 0) {
        if (!file_.resize(0) || !file_.seek(0)) { fail("Cannot restart a server download that does not support Range."); return false; }
        offset_ = 0;
    }
    if (status == 206) {
        static const QRegularExpression range("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$");
        const auto match = range.match(QString::fromLatin1(reply_->rawHeader("Content-Range")));
        if (!match.hasMatch() || match.captured(1).toLongLong() != offset_ || match.captured(3).toLongLong() != asset_.size ||
            match.captured(2).toLongLong() < offset_ || match.captured(2).toLongLong() >= asset_.size) {
            fail("Model server returned an invalid resume range. No model was installed.");
            return false;
        }
    }
    const auto encoding = reply_->rawHeader("Content-Encoding").toLower();
    if (!encoding.isEmpty() && encoding != "identity") {
        fail("Model server returned an unsupported content encoding.");
        return false;
    }
    const auto length = reply_->header(QNetworkRequest::ContentLengthHeader);
    if (length.isValid() && length.toLongLong() != asset_.size - offset_) {
        fail("Model server returned an unexpected component size.");
        return false;
    }
    QByteArray etag = reply_->rawHeader("ETag");
    if (etag.contains('\r') || etag.contains('\n') || etag.size() > 1024) etag.clear();
    const auto data = QJsonDocument(QJsonObject{{"url", asset_.url.toString()}, {"size", double(asset_.size)},
                                               {"sha256", QString::fromLatin1(asset_.sha256.toHex())}, {"etag", QString::fromUtf8(etag)}}).toJson();
    QSaveFile metadata(metadata_);
    if (!metadata.open(QIODevice::WriteOnly) || metadata.write(data) != data.size() || !metadata.commit()) {
        fail("Cannot save model resume metadata.");
        return false;
    }
    headers_ = true;
    return true;
}
void AiModelDownloader::receive() {
    if (!acceptResponse() || !reply_) return;
    while (active_ && reply_ && reply_->bytesAvailable() > 0) {
        const auto bytes = reply_->read(1024 * 1024);
        if (bytes.isEmpty()) break;
        if (file_.pos() + bytes.size() > asset_.size) { fail("Download exceeds the catalog's model size."); return; }
        if (file_.write(bytes) != bytes.size()) { fail("Cannot write model cache. Check free disk space."); return; }
        emit progress(file_.pos(), asset_.size);
    }
}
void AiModelDownloader::finishNetwork() {
    if (!active_ || !reply_) return;
    if (cancelRequested_) { finishCancelled(); return; }
    if (reply_->error() != QNetworkReply::NoError) {
        fail("Cannot download the model. Check your network connection; installed models remain usable offline. " + reply_->errorString());
        return;
    }
    receive();
    if (!active_) return;
    if (!headers_) { fail("Model server did not provide a valid download response."); return; }
    file_.close();
    reply_->deleteLater();
    reply_ = nullptr;
    if (QFileInfo(part_).size() != asset_.size) {
        fail("Model download was interrupted. The partial file is preserved for Resume.");
        return;
    }
    verifyAndInstall();
}
void AiModelDownloader::verifyAndInstall() {
    auto watcher = new QFutureWatcher<AiModelInstallResult>(this);
    connect(watcher, &QFutureWatcher<AiModelInstallResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (result.cancelled || cancelRequested_) { finishCancelled(); return; }
        if (!result.success) { fail(result.error); return; }
        active_ = false;
        installLock_.reset();
        const auto installed = destination_;
        emit finished(installed);
    });
    watcher->setFuture(QtConcurrent::run([asset = asset_, part = part_, destination = destination_, metadata = metadata_, cancellation = cancellation_, installLock = installLock_] {
        try {
            require(verifyAiAsset(part, asset, cancellation), "Downloaded model size/SHA256 check failed. Use Restart; no unverified model was installed.");
            cancellation->check();
            if (!verifyAiAsset(destination, asset, cancellation)) {
                QFile input(part);
                QSaveFile output(destination);
                require(input.open(QIODevice::ReadOnly) && output.open(QIODevice::WriteOnly), "Cannot install verified model");
                QCryptographicHash hash(QCryptographicHash::Sha256);
                qint64 total = 0;
                while (!input.atEnd()) {
                    cancellation->check();
                    const auto bytes = input.read(1024 * 1024);
                    require(!bytes.isEmpty() || input.error() == QFileDevice::NoError, "Cannot read verified model");
                    total += bytes.size();
                    hash.addData(bytes);
                    require(output.write(bytes) == bytes.size(), "Cannot install model cache");
                }
                require(total == asset.size && hash.result() == asset.sha256, "Model changed during installation");
                cancellation->check();
                require(output.commit(), "Cannot commit verified model");
            }
            QFile::remove(part);
            QFile::remove(metadata);
            return AiModelInstallResult{true, false, {}};
        } catch (const AiCancelled &) {
            return AiModelInstallResult{false, true, {}};
        } catch (const std::exception &error) {
            return AiModelInstallResult{false, false, QString::fromUtf8(error.what())};
        }
    }));
}
void AiModelDownloader::cancel() {
    if (!active_) return;
    cancelRequested_ = true;
    cancellation_->cancel();
    if (reply_) reply_->abort();
}
void AiModelDownloader::finishCancelled() {
    if (!active_) return;
    active_ = false;
    installLock_.reset();
    file_.close();
    if (reply_) { reply_->disconnect(this); reply_->deleteLater(); reply_ = nullptr; }
    emit cancelled();
}
void AiModelDownloader::fail(const QString &message) {
    active_ = false;
    if (cancellation_) cancellation_->cancel();
    installLock_.reset();
    file_.close();
    if (reply_) { reply_->disconnect(this); reply_->abort(); reply_->deleteLater(); reply_ = nullptr; }
    emit failed(message);
}
}
