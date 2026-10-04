#pragma once
#include "ai_models.h"
#include <QFile>
#include <QObject>
#include <QPointer>
class QNetworkAccessManager;
class QNetworkReply;

namespace compositor {
class AiModelDownloader : public QObject {
    Q_OBJECT
  public:
    explicit AiModelDownloader(QString cacheRoot = {}, QObject *parent = nullptr,
                               QNetworkAccessManager *network = nullptr);
    ~AiModelDownloader() override;
    bool busy() const;
    void start(const AiModelDefinition &model, int assetIndex, bool restart = false);
    void cancel();
  signals:
    void progress(qint64 received, qint64 total);
    void finished(const QString &path);
    void failed(const QString &message);
    void cancelled();
  private:
    bool acceptResponse();
    void receive();
    void finishNetwork();
    void verifyAndInstall();
    void fail(const QString &message);
    void finishCancelled();
    QString root_, destination_, part_, metadata_;
    AiModelDefinition model_;
    AiModelAsset asset_;
    QNetworkAccessManager *network_;
    QPointer<QNetworkReply> reply_;
    QFile file_;
    std::shared_ptr<QLockFile> installLock_;
    std::shared_ptr<AiCancellation> cancellation_;
    qint64 offset_ = 0;
    bool active_ = false, headers_ = false, cancelRequested_ = false;
};
}
