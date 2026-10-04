#pragma once
#include "ai_session.h"
#include <QImage>
#include <QPointF>
#include <QSize>

namespace compositor {
enum class AiImageKind { BiRefNet, SAM2, MobileSAM };
struct AiPreparedImage {
    AiTensor tensor;
    QSize originalSize;
    QSize resizedSize;
};
AiPreparedImage prepareAiImage(const QImage &image, AiImageKind kind,
                              std::shared_ptr<AiCancellation> cancellation = {}, int side = 1024);
QPointF convertAiPoint(QPointF point, const AiPreparedImage &image, AiImageKind kind, int side = 1024);
QVector<float> aiProbabilities(const AiTensor &logits);
}
