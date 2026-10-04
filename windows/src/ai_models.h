#pragma once
#include "ai_session.h"
#include <QMap>
#include <QUrl>
#include <QLockFile>

namespace compositor {
struct AiModelAsset {
    QString filename;
    QUrl url;
    qint64 size = 0;
    QByteArray sha256;
};
struct AiModelDefinition {
    QString id, name, license, source;
    bool published = false;
    QList<AiModelAsset> assets;
};
struct AiModelState {
    bool installed = false;
    QString message;
};
struct AiModelInstallResult {
    bool success = false, cancelled = false;
    QString error;
};
const QList<AiModelDefinition> &aiModelCatalog();
QByteArray aiModelCatalogJson();
QString aiModelCacheRoot();
QString aiModelPath(const AiModelDefinition &model, const AiModelAsset &asset, const QString &root = {});
std::shared_ptr<QLockFile> lockAiModelInstall(const AiModelDefinition &model, const QString &root = {});
bool verifyAiAsset(const QString &path, const AiModelAsset &asset, std::shared_ptr<AiCancellation> cancellation = {});
QMap<QString, QByteArray> aiModelLicenseFiles(const AiModelDefinition &model);
void saveAiModelLicenses(const AiModelDefinition &model, const QString &root = {});
AiModelState inspectAiModel(const AiModelDefinition &model, const QString &root = {}, std::shared_ptr<AiCancellation> cancellation = {});
QFuture<AiModelState> inspectAiModelAsync(AiModelDefinition model, QString root = {}, std::shared_ptr<AiCancellation> cancellation = {});
QFuture<AiModelInstallResult> importAiModelAsync(AiModelDefinition model, QString sourceDirectory, QString root = {},
                                              std::shared_ptr<AiCancellation> cancellation = {});
}
