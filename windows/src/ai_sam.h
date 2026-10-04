#pragma once
#include "ai_preprocess.h"
#include <memory>
#include <mutex>

namespace compositor {
struct AiEncodedImage {
    AiPreparedImage geometry;
    QList<AiTensor> features;
    AiRunResult encoderResult;
    qint64 imageKey = 0;
};
class AiSamModel : public std::enable_shared_from_this<AiSamModel> {
  public:
    static std::shared_ptr<AiSamModel> open(QString encoder, QString decoder, AiImageKind kind,
                                          const AiOptions &options = {});
    static QFuture<std::shared_ptr<AiSamModel>> openAsync(QString encoder, QString decoder,
                                                       AiImageKind kind, AiOptions options = {});
    std::shared_ptr<const AiEncodedImage> encodeOnce(const QImage &image,
                                                   std::shared_ptr<AiCancellation> cancellation = {});
    QFuture<std::shared_ptr<const AiEncodedImage>> encodeAsync(QImage image,
                                                             std::shared_ptr<AiCancellation> cancellation = {});
    AiRunResult decode(const std::shared_ptr<const AiEncodedImage> &image, const QVector<QPointF> &points,
                       const QVector<int> &labels, AiTensor previousMask = {},
                       std::shared_ptr<AiCancellation> cancellation = {});
    QFuture<AiRunResult> decodeAsync(std::shared_ptr<const AiEncodedImage> image, QVector<QPointF> points,
                                    QVector<int> labels, AiTensor previousMask = {},
                                    std::shared_ptr<AiCancellation> cancellation = {});
    quint64 encoderRuns() const;
    void clearCache();
    std::shared_ptr<AiSession> encoderSession() const;
    std::shared_ptr<AiSession> decoderSession() const;
  private:
    AiSamModel() = default;
    AiImageKind kind_ = AiImageKind::SAM2;
    std::shared_ptr<AiSession> encoder_, decoder_;
    std::shared_ptr<const AiEncodedImage> cached_;
    mutable std::mutex mutex_;
    quint64 encoderRuns_ = 0;
};
}
