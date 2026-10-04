// SPDX-License-Identifier: MIT
#pragma once
#include "ai_sam.h"
namespace compositor {
struct AiSelectionResult {
    QImage mask;
    QString backend, adapter, fallbackReason;
    double milliseconds = 0;
    int candidate = -1;
    quint64 encoderRuns = 0;
};
// UI-independent service; tests can supply an implementation without model weights.
class AiSelectionService {
  public:
    virtual ~AiSelectionService() = default;
    virtual AiSelectionResult subject(const QImage &, std::shared_ptr<AiCancellation>) = 0;
    virtual AiSelectionResult object(const QImage &, const QString &model,
                                     const QVector<QPointF> &, const QVector<int> &,
                                     std::shared_ptr<AiCancellation>) = 0;
};
std::shared_ptr<AiSelectionService> createAiSelectionService(QString modelRoot = {}, AiOptions options = {});
QImage aiSubjectSelection(const AiTensor &logits, const QImage &image,
                          std::shared_ptr<AiCancellation> cancellation = {});
AiSelectionResult aiObjectSelection(const AiRunResult &, const QImage &, AiImageKind,
                                    std::shared_ptr<AiCancellation> cancellation = {});
QImage combineAiSelection(const QImage &original, const QImage &mask, int mode);
} // namespace compositor
