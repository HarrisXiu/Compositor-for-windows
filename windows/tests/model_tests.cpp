#include "ai_download.h"
#include "ai_models_dialog.h"
#include "editor.h"
#include "language.h"
#include "shortcuts.h"
#include <QAction>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <algorithm>
#include <cstring>

using namespace compositor;
namespace {
const QByteArray payload("0123456789");
AiModelDefinition fixture() {
    return {"fixture", "Fixture", "MIT", "local-test", true,
            {{"fixture.onnx", QUrl("https://models.invalid/fixture.onnx"), payload.size(),
              QCryptographicHash::hash(payload, QCryptographicHash::Sha256)}}};
}
bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
struct Response {
    int status = 200;
    QByteArray data = payload, range, encoding;
    qint64 length = payload.size();
    int chunk = 0;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
};
class Reply : public QNetworkReply {
    Response response_;
    QByteArray available_;
    qsizetype offset_ = 0;
    void pump() {
        if (isFinished()) return;
        if (!offset_) {
            if (response_.status) setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response_.status);
            if (response_.length >= 0) setHeader(QNetworkRequest::ContentLengthHeader, response_.length);
            if (!response_.range.isEmpty()) setRawHeader("Content-Range", response_.range);
            if (!response_.encoding.isEmpty()) setRawHeader("Content-Encoding", response_.encoding);
            setRawHeader("ETag", "\"fixture-v1\"");
            emit metaDataChanged();
            if (isFinished()) return;
        }
        if (response_.error != NoError) {
            setError(response_.error, "Simulated offline connection");
            setFinished(true);
            emit finished();
            return;
        }
        const auto count = response_.chunk ? response_.chunk : response_.data.size();
        available_.append(response_.data.mid(offset_, count));
        offset_ += count;
        emit readyRead();
        if (isFinished()) return;
        if (offset_ >= response_.data.size()) {
            setFinished(true);
            emit finished();
        } else {
            QTimer::singleShot(10, this, [this] { pump(); });
        }
    }
  public:
    Reply(const QNetworkRequest &request, Response response, QObject *parent)
        : QNetworkReply(parent), response_(std::move(response)) {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] { pump(); });
    }
    void abort() override {
        if (isFinished()) return;
        setError(OperationCanceledError, "Cancelled");
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override { return available_.size() + QNetworkReply::bytesAvailable(); }
  protected:
    qint64 readData(char *data, qint64 maximum) override {
        const auto size = std::min(maximum, qint64(available_.size()));
        std::memcpy(data, available_.constData(), size_t(size));
        available_.remove(0, size);
        return size;
    }
};
class Network : public QNetworkAccessManager {
  public:
    Response response;
    QList<QNetworkRequest> requests;
  protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *) override {
        Q_ASSERT(operation == GetOperation);
        requests.append(request);
        return new Reply(request, response, this);
    }
};
void partial(const AiModelDefinition &model, const QString &root, QByteArray hash = {}) {
    const auto path = aiModelPath(model, model.assets.first(), root);
    writeFile(path + ".part", payload.left(3));
    const auto &asset = model.assets.first();
    writeFile(path + ".resume.json", QJsonDocument(QJsonObject{
        {"size", double(asset.size)}, {"sha256", QString::fromLatin1(hash.isEmpty() ? asset.sha256.toHex() : hash)},
        {"url", asset.url.toString()}, {"etag", "\"fixture-v1\""}}).toJson());
}
}
class ModelTests : public QObject {
    Q_OBJECT
  private slots:
    void pinnedHttpsTransfer() {
        if (!qEnvironmentVariableIsSet("COMPOSITOR_AI_HTTPS_TEST"))
            QSKIP("Optional live HTTPS check; offline regressions cover transfer states.");
        QTemporaryDir dir;
        const AiModelDefinition model{"https-fixture", "HTTPS fixture", "MIT", "microsoft/onnxruntime@v1.24.4", true,
            {{"license.txt", QUrl("https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/LICENSE"), 1073,
              QByteArray::fromHex("2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c")}}};
        AiModelDownloader downloader(dir.path());
        QSignalSpy done(&downloader, &AiModelDownloader::finished), errors(&downloader, &AiModelDownloader::failed);
        downloader.start(model, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !errors.isEmpty(), 90000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
        QCOMPARE(done.size(), 1);
        QVERIFY(verifyAiAsset(aiModelPath(model, model.assets.first(), dir.path()), model.assets.first()));
    }
    void completeDownloadAndOfflineIntegrity() {
        QTemporaryDir dir;
        Network network;
        AiModelDownloader downloader(dir.path(), nullptr, &network);
        QSignalSpy done(&downloader, &AiModelDownloader::finished), errors(&downloader, &AiModelDownloader::failed);
        const auto model = fixture();
        downloader.start(model, 0);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(errors.size(), 0);
        QVERIFY(!downloader.busy());
        const auto path = aiModelPath(model, model.assets.first(), dir.path());
        QCOMPARE(readFile(path), payload);
        QVERIFY(!QFileInfo::exists(path + ".part"));
        saveAiModelLicenses(model, dir.path());
        QVERIFY(inspectAiModel(model, dir.path()).installed);
        QVERIFY(writeFile(path, QByteArray("x123456789")));
        QVERIFY(!inspectAiModel(model, dir.path()).installed);
    }
    void resumeUsesRangeAndStrongEtag() {
        QTemporaryDir dir;
        const auto model = fixture();
        partial(model, dir.path());
        Network network;
        network.response = {206, payload.mid(3), "bytes 3-9/10", {}, 7};
        AiModelDownloader downloader(dir.path(), nullptr, &network);
        QSignalSpy done(&downloader, &AiModelDownloader::finished);
        downloader.start(model, 0);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(network.requests.first().rawHeader("Range"), QByteArray("bytes=3-"));
        QCOMPARE(network.requests.first().rawHeader("If-Range"), QByteArray("\"fixture-v1\""));
        QCOMPARE(readFile(aiModelPath(model, model.assets.first(), dir.path())), payload);
    }
    void ignoredRangeRestartsSafely() {
        QTemporaryDir dir;
        const auto model = fixture();
        partial(model, dir.path());
        Network network;
        AiModelDownloader downloader(dir.path(), nullptr, &network);
        QSignalSpy done(&downloader, &AiModelDownloader::finished);
        downloader.start(model, 0);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(readFile(aiModelPath(model, model.assets.first(), dir.path())), payload);
    }
    void cancellationPreservesResumeAndReleasesLock() {
        QTemporaryDir dir;
        const auto model = fixture();
        Network network;
        network.response.chunk = 3;
        AiModelDownloader downloader(dir.path(), nullptr, &network);
        QSignalSpy cancelled(&downloader, &AiModelDownloader::cancelled), done(&downloader, &AiModelDownloader::finished);
        const auto connection = connect(&downloader, &AiModelDownloader::progress, &downloader, [&](qint64 received, qint64) {
            if (received >= 3) downloader.cancel();
        });
        downloader.start(model, 0);
        QTRY_COMPARE(cancelled.size(), 1);
        disconnect(connection);
        const auto path = aiModelPath(model, model.assets.first(), dir.path());
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(readFile(path + ".part"), payload.left(3));
        QVERIFY(QFileInfo::exists(path + ".resume.json"));
        network.response = {206, payload.mid(3), "bytes 3-9/10", {}, 7};
        downloader.start(model, 0);
        QTRY_COMPARE(done.size(), 1);
    }
    void invalidResponses_data() {
        QTest::addColumn<Response>("response");
        QTest::newRow("HTTP-error") << Response{404, {}, {}, {}, 0};
        QTest::newRow("wrong-length") << Response{200, payload, {}, {}, 11};
        QTest::newRow("invalid-range") << Response{206, payload, "bytes 2-9/10"};
        QTest::newRow("compressed") << Response{200, payload, {}, "gzip"};
        QTest::newRow("truncated") << Response{200, payload.left(5), {}, {}, -1};
        QTest::newRow("oversized") << Response{200, payload + "x", {}, {}, -1};
        QTest::newRow("SHA256") << Response{200, QByteArray("x123456789")};
        QTest::newRow("offline") << Response{0, {}, {}, {}, -1, 0, QNetworkReply::HostNotFoundError};
    }
    void invalidResponses() {
        QFETCH(Response, response);
        QTemporaryDir dir;
        const auto model = fixture();
        const auto path = aiModelPath(model, model.assets.first(), dir.path());
        QVERIFY(writeFile(path, payload));
        saveAiModelLicenses(model, dir.path());
        Network network;
        network.response = response;
        AiModelDownloader downloader(dir.path(), nullptr, &network);
        QSignalSpy errors(&downloader, &AiModelDownloader::failed), done(&downloader, &AiModelDownloader::finished);
        downloader.start(model, 0);
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(done.size(), 0);
        QVERIFY(!downloader.busy());
        QCOMPARE(readFile(path), payload);
        QVERIFY(inspectAiModel(model, dir.path()).installed);
    }
    void mismatchedPartialRequiresExplicitRestart() {
        QTemporaryDir dir;
        const auto model = fixture();
        partial(model, dir.path(), QByteArray(64, '0'));
        Network network;
        AiModelDownloader downloader(dir.path(), nullptr, &network);
        QSignalSpy errors(&downloader, &AiModelDownloader::failed), done(&downloader, &AiModelDownloader::finished);
        downloader.start(model, 0);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(network.requests.size(), 0);
        downloader.start(model, 0, true);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(network.requests.first().rawHeader("Range").isEmpty());
    }
    void concurrentInstallIsRejected() {
        QTemporaryDir dir;
        const auto model = fixture();
        const auto lock = lockAiModelInstall(model, dir.path());
        Network network;
        AiModelDownloader downloader(dir.path(), nullptr, &network);
        QSignalSpy errors(&downloader, &AiModelDownloader::failed);
        downloader.start(model, 0);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(network.requests.size(), 0);
        auto imported = importAiModelAsync(model, dir.path(), dir.path());
        imported.waitForFinished();
        QVERIFY(!imported.result().success);
        QVERIFY(imported.result().error.contains("Another process"));
    }
    void localImportCancellationAndLicenseRepair() {
        QTemporaryDir source, cache;
        const auto model = fixture();
        QVERIFY(writeFile(source.filePath("fixture.onnx"), payload));
        auto cancelled = std::make_shared<AiCancellation>();
        cancelled->cancel();
        auto job = importAiModelAsync(model, source.path(), cache.path(), cancelled);
        job.waitForFinished();
        QVERIFY(job.result().cancelled);
        QVERIFY(!QFileInfo::exists(aiModelPath(model, model.assets.first(), cache.path())));
        job = importAiModelAsync(model, source.path(), cache.path());
        job.waitForFinished();
        QVERIFY2(job.result().success, qPrintable(job.result().error));
        QVERIFY(inspectAiModel(model, cache.path()).installed);
        const auto license = cache.filePath("fixture/LICENSE.txt");
        auto text = aiModelLicenseFiles(model).value("LICENSE.txt");
        text.replace("\r\n", "\n");
        QVERIFY(writeFile(license, text));
        QVERIFY(inspectAiModel(model, cache.path()).installed);
        text.replace("\n", "\r\n");
        QVERIFY(writeFile(license, text));
        QVERIFY(inspectAiModel(model, cache.path()).installed);
        QVERIFY(writeFile(license, "wrong license"));
        QVERIFY(!inspectAiModel(model, cache.path()).installed);
        job = importAiModelAsync(model, source.path(), cache.path());
        job.waitForFinished();
        QVERIFY(job.result().success);
        QVERIFY(inspectAiModel(model, cache.path()).installed);
        QVERIFY(writeFile(source.filePath("fixture.onnx"), "x123456789"));
        job = importAiModelAsync(model, source.path(), cache.path());
        job.waitForFinished();
        QVERIFY(!job.result().success);
        QVERIFY(inspectAiModel(model, cache.path()).installed);
    }
    void unpublishedAndUnsafeAssetsNeverRequestNetwork() {
        QTemporaryDir dir;
        auto model = fixture();
        model.published = false;
        Network network;
        AiModelDownloader downloader(dir.path(), nullptr, &network);
        QSignalSpy errors(&downloader, &AiModelDownloader::failed);
        downloader.start(model, 0);
        QCOMPARE(errors.size(), 1);
        model.published = true;
        model.assets[0].url = QUrl("http://models.invalid/file");
        downloader.start(model, 0);
        QCOMPARE(errors.size(), 2);
        model.assets[0].url = QUrl("https://models.invalid/file");
        model.id = "../escape";
        downloader.start(model, 0);
        QCOMPARE(errors.size(), 3);
        QCOMPARE(network.requests.size(), 0);
    }
    void dialogMenuAndPublicationState() {
        UiLanguage::instance().setLanguage("en", false);
        QTemporaryDir dir;
        AiModelsDialog dialog(nullptr, dir.path());
        for (const auto &model : aiModelCatalog()) {
            QVERIFY(!model.published);
            const auto download = dialog.findChild<QPushButton *>("download_" + model.id);
            const auto import = dialog.findChild<QPushButton *>("import_" + model.id);
            QVERIFY(download && !download->isEnabled());
            QVERIFY(import && import->isEnabled());
        }
        const auto screenshot = qEnvironmentVariable("COMPOSITOR_AI_DIALOG_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            dialog.show();
            QTest::qWait(30);
            QVERIFY(dialog.grab().save(screenshot));
            dialog.hide();
        }
        EditorWindow window;
        const auto action = window.findChild<QAction *>("aiModelsAction");
        QVERIFY(action);
        const auto &entries = Shortcuts::instance().entries();
        QVERIFY(std::any_of(entries.cbegin(), entries.cend(), [](const ShortcutEntry &entry) {
            return entry.id == "menu/&Help/AI Models…";
        }));
        QVERIFY(std::any_of(entries.cbegin(), entries.cend(), [](const ShortcutEntry &entry) {
            return entry.id == "menu/&Edit/Keyboard Shortcuts…";
        }));
        for (const auto &language : {QString("zh_CN"), QString("ja_JP")}) {
            UiLanguage::instance().setLanguage(language, false);
            UiLanguage::instance().translateObject(&window, true);
            UiLanguage::instance().translateObject(&dialog, true);
            QCOMPARE(action->text(), uiText("AI Models…"));
            QCOMPARE(dialog.windowTitle(), uiText("AI Models"));
            QVERIFY(action->text() != "AI Models…");
            QVERIFY(uiText("Keyboard Shortcuts…") != "Keyboard Shortcuts…");
        }
        UiLanguage::instance().setLanguage("en", false);
        bool opened = false;
        QTimer::singleShot(10, &window, [&] {
            const auto modal = qobject_cast<AiModelsDialog *>(QApplication::activeModalWidget());
            if (modal) { opened = true; modal->reject(); }
        });
        action->trigger();
        QVERIFY(opened);
    }
};
Q_DECLARE_METATYPE(Response)
QTEST_MAIN(ModelTests)
#include "model_tests.moc"
