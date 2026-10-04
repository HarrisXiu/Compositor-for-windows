#include "ai_models.h"
#include "document.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

static void initializeAiLicenses() {
    static const bool initialized = [] { Q_INIT_RESOURCE(ai_licenses); return true; }();
    Q_UNUSED(initialized);
}
namespace compositor {
namespace {
AiModelAsset asset(QString name, qint64 size, const char *hash) {
    return {name, QUrl("https://github.com/HarrisXiu/Compositor-for-windows/releases/download/ai-models-v1/" + name), size, QByteArray::fromHex(hash)};
}
void safeName(const AiModelDefinition &model, const AiModelAsset &file) {
    static const QRegularExpression identifier("^[a-z0-9][a-z0-9-]*$"), filename("^[A-Za-z0-9][A-Za-z0-9_.-]*$");
    require(identifier.match(model.id).hasMatch() && filename.match(file.filename).hasMatch() && !file.filename.contains(".."), "Unsafe AI cache filename");
    require(file.size > 0 && file.size <= 2LL * 1024 * 1024 * 1024 && file.sha256.size() == 32, "Invalid AI model integrity metadata");
}
}
const QList<AiModelDefinition> &aiModelCatalog() {
    static const QList<AiModelDefinition> catalog{
        {"birefnet-lite", "BiRefNet Lite (FP32)", "MIT", "ZhengPeng7/BiRefNet_lite@aa62cd87eafb9cc43056d08ef3615a14628b831d", false,
         {asset("birefnet-lite.onnx", 181695409, "e40167bbaf9b3bb0cf64ebd700dff1b9431da6161ccaee9062a95fc5b16270d7")}},
        {"sam2", "SAM 2 Hiera Tiny (FP32)", "Apache-2.0", "facebook/sam2-hiera-tiny@7c218beaf0bb87874785f32b582f640134fc1c09", false,
         {asset("sam2_encoder.onnx", 109471958, "461ce21868f57db114211d09d2c853bc02e7e9a3034cdab71d00a81d3a0767a5"),
          asset("sam2_decoder.onnx", 16564159, "0f57537980e2c077e34b820919ef999e9449709dac17a6cd11e212c0a8fec000")}},
        {"mobilesam", "MobileSAM (FP32)", "Apache-2.0", "ChaoningZhang/MobileSAM@f706ad9c4eb7f219c00d9050e46328518ffb65d2", false,
         {asset("mobilesam_encoder.onnx", 27969729, "c92cd14ef8c41e9b793c7b54c1beb106732ced449039dd19badbf99c8d0bab72"),
          asset("mobilesam_decoder.onnx", 16496929, "8f269b4e837d13e69d50d4104107cb4f9c7406a48571eb40e112f10eb722da2d")}}
    };
    return catalog;
}
QByteArray aiModelCatalogJson() {
    QJsonArray models;
    for (const auto &model : aiModelCatalog()) {
        QJsonArray assets;
        for (const auto &file : model.assets)
            assets.append(QJsonObject{{"filename", file.filename}, {"url", file.url.toString()},
                                      {"bytes", double(file.size)}, {"sha256", QString::fromLatin1(file.sha256.toHex())}});
        models.append(QJsonObject{{"id", model.id}, {"name", model.name}, {"license", model.license},
                                  {"source", model.source}, {"published", model.published}, {"assets", assets}});
    }
    return QJsonDocument(QJsonObject{{"version", 1}, {"release_tag", "ai-models-v1"}, {"models", models}}).toJson();
}
QString aiModelCacheRoot() {
    const auto local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    require(!local.isEmpty(), "Windows local application data directory is unavailable");
    return QDir(local).filePath("Compositor/Models");
}
QString aiModelPath(const AiModelDefinition &model, const AiModelAsset &file, const QString &root) {
    safeName(model, file);
    return QDir(root.isEmpty() ? aiModelCacheRoot() : root).filePath(model.id + "/" + file.filename);
}
std::shared_ptr<QLockFile> lockAiModelInstall(const AiModelDefinition &model, const QString &root) {
    require(!model.assets.isEmpty(), "AI model has no assets");
    const auto directory = QFileInfo(aiModelPath(model, model.assets.first(), root)).absolutePath();
    require(QDir().mkpath(directory) && !QFileInfo(directory).isSymLink(), "Unsafe or unwritable model cache");
    const auto path = QDir(directory).filePath(".install.lock");
    require(!QFileInfo(path).isSymLink(), "Unsafe model install lock");
    auto lock = std::make_shared<QLockFile>(path);
    lock->setStaleLockTime(0);
    require(lock->tryLock(), "Another process is installing this model. Try again when it finishes.");
    return lock;
}
bool verifyAiAsset(const QString &path, const AiModelAsset &file, std::shared_ptr<AiCancellation> cancellation) {
    QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() != file.size)
        return false;
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!input.atEnd()) {
        if (cancellation) cancellation->check();
        const auto bytes = input.read(1024 * 1024);
        if (bytes.isEmpty() && input.error() != QFileDevice::NoError)
            return false;
        hash.addData(bytes);
    }
    if (cancellation) cancellation->check();
    return hash.result() == file.sha256;
}
QMap<QString, QByteArray> aiModelLicenseFiles(const AiModelDefinition &model) {
    initializeAiLicenses();
    require(model.license == "MIT" || model.license == "Apache-2.0", "Unsupported model license");
    QFile license(model.license == "MIT" ? ":/ai-licenses/BiRefNet-MIT.txt" : ":/ai-licenses/Apache-2.0.txt");
    require(license.open(QIODevice::ReadOnly), "Bundled AI model license is missing");
    QByteArray notice = (model.name + "\nLicense: " + model.license + "\nSource: " + model.source + "\n").toUtf8();
    if (model.id == "sam2")
        notice += "Copyright (c) Meta Platforms, Inc. and affiliates.\nONNX wrappers: Copyright (c) Microsoft Corporation, MIT license.\nExported for Compositor as separate FP32 ONNX encoder/decoder graphs; trained weights are unchanged.\n";
    else if (model.id == "mobilesam")
        notice += "MobileSAM by Chaoning Zhang and contributors; based on Segment Anything.\nCopyright (c) Meta Platforms, Inc. and affiliates.\nExported for Compositor as separate FP32 ONNX encoder/decoder graphs; trained weights are unchanged.\n";
    else
        notice += "Copyright (c) 2024 ZhengPeng.\nExport-only deformable convolution lowered to standard GridSample/MatMul operations; trained weights are unchanged.\n";
    QMap<QString, QByteArray> documents{{"LICENSE.txt", license.readAll()}, {"NOTICE.txt", notice}};
    if (model.id == "sam2") {
        QFile wrappers(":/ai-licenses/Microsoft-MIT.txt");
        require(wrappers.open(QIODevice::ReadOnly), "ONNX exporter license is missing");
        documents["LICENSE-ONNX-exporter.txt"] = wrappers.readAll();
    }
    return documents;
}
void saveAiModelLicenses(const AiModelDefinition &model, const QString &root) {
    require(!model.assets.isEmpty(), "AI model has no assets");
    const auto directory = QFileInfo(aiModelPath(model, model.assets.first(), root)).absolutePath();
    require(QDir().mkpath(directory), "Cannot create model license directory");
    const auto documents = aiModelLicenseFiles(model);
    for (auto it = documents.begin(); it != documents.end(); ++it) {
        QSaveFile file(QDir(directory).filePath(it.key()));
        require(file.open(QIODevice::WriteOnly) && file.write(it.value()) == it.value().size() && file.commit(), "Cannot save AI model license");
    }
}
AiModelState inspectAiModel(const AiModelDefinition &model, const QString &root, std::shared_ptr<AiCancellation> cancellation) {
    for (const auto &file : model.assets)
        if (!verifyAiAsset(aiModelPath(model, file, root), file, cancellation))
            return {false, "Model is missing or its size/SHA256 check failed."};
    require(!model.assets.isEmpty(), "AI model has no assets");
    const auto directory = QFileInfo(aiModelPath(model, model.assets.first(), root)).absolutePath();
    const auto documents = aiModelLicenseFiles(model);
    for (auto it = documents.begin(); it != documents.end(); ++it) {
        QFile file(QDir(directory).filePath(it.key()));
        if (!file.open(QIODevice::ReadOnly))
            return {false, "Model license files are missing or do not match the installed model."};
        // Git checkouts and portable imports can use different text line endings.
        // Preserve the complete license/notice content while comparing LF and CRLF equally.
        auto installed = file.readAll(), expected = it.value();
        if (installed.replace("\r\n", "\n") != expected.replace("\r\n", "\n"))
            return {false, "Model license files are missing or do not match the installed model."};
    }
    return {true, "Installed and SHA256 verified. Available offline."};
}
QFuture<AiModelState> inspectAiModelAsync(AiModelDefinition model, QString root, std::shared_ptr<AiCancellation> cancellation) {
    return QtConcurrent::run([model, root, cancellation] { return inspectAiModel(model, root, cancellation); });
}
QFuture<AiModelInstallResult> importAiModelAsync(AiModelDefinition model, QString sourceDirectory, QString root,
                                             std::shared_ptr<AiCancellation> cancellation) {
    if (!cancellation) cancellation = std::make_shared<AiCancellation>();
    return QtConcurrent::run([model, sourceDirectory, root, cancellation] {
        try {
            const auto installLock = lockAiModelInstall(model, root);
            for (const auto &file : model.assets) {
                cancellation->check();
                const auto source = QDir(sourceDirectory).filePath(file.filename);
                require(verifyAiAsset(source, file, cancellation), "Local model size/SHA256 does not match the built-in catalog: " + file.filename);
                const auto destination = aiModelPath(model, file, root);
                if (verifyAiAsset(destination, file, cancellation)) continue;
                require(QDir().mkpath(QFileInfo(destination).absolutePath()) && !QFileInfo(destination).isSymLink(), "Unsafe or unwritable AI cache directory");
                QFile input(source);
                QSaveFile output(destination);
                require(input.open(QIODevice::ReadOnly) && output.open(QIODevice::WriteOnly), "Cannot import AI model");
                QCryptographicHash hash(QCryptographicHash::Sha256);
                qint64 total = 0;
                while (!input.atEnd()) {
                    cancellation->check();
                    const auto bytes = input.read(1024 * 1024);
                    require(!bytes.isEmpty() || input.error() == QFileDevice::NoError, "Cannot read local AI model");
                    total += bytes.size();
                    hash.addData(bytes);
                    require(output.write(bytes) == bytes.size(), "Cannot write AI model cache");
                }
                require(total == file.size && hash.result() == file.sha256, "Local model changed during import");
                cancellation->check();
                require(output.commit(), "Cannot install local AI model");
            }
            saveAiModelLicenses(model, root);
            return AiModelInstallResult{true, false, {}};
        } catch (const AiCancelled &) {
            return AiModelInstallResult{false, true, {}};
        } catch (const std::exception &error) {
            return AiModelInstallResult{false, false, QString::fromUtf8(error.what())};
        }
    });
}
}
