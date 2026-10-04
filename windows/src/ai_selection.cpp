// SPDX-License-Identifier: MIT
#include "ai_selection.h"
#include "ai_models.h"
#include "document.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <map>
namespace compositor {
namespace {
void check(const std::shared_ptr<AiCancellation> &cancellation) {
    if (cancellation)
        cancellation->check();
}
const AiModelDefinition &definition(const QString &id) {
    for (const auto &model : aiModelCatalog())
        if (model.id == id)
            return model;
    throw Error("Unknown AI selection model");
}
class SelectionService final : public AiSelectionService {
  public:
    SelectionService(QString root, AiOptions options) : root_(std::move(root)), options_(options) {}
    AiSelectionResult subject(const QImage &image, std::shared_ptr<AiCancellation> cancellation) override {
        return segment(image, cancellation, true);
    }
    AiSelectionResult matte(const QImage &image, std::shared_ptr<AiCancellation> cancellation) override {
        return segment(image, cancellation, false);
    }
    AiSelectionResult segment(const QImage &image, std::shared_ptr<AiCancellation> cancellation, bool binary) {
        std::lock_guard lock(mutex_);
        check(cancellation);
        QElapsedTimer timer;
        timer.start();
        if (!subject_) {
            const auto &model = verified("birefnet-lite", cancellation);
            subject_ = AiSession::open(aiModelPath(model, model.assets.first(), root_), options_);
        }
        check(cancellation);
        auto prepared = prepareAiImage(image, AiImageKind::BiRefNet, cancellation);
        auto run = subject_->run({prepared.tensor}, cancellation);
        require(run.outputs.size() == 1 && run.outputs.first().name == "logits",
                "BiRefNet returned an unexpected output contract");
        AiSelectionResult result;
        result.mask = binary ? aiSubjectSelection(run.outputs.first(), image, cancellation)
                             : aiSubjectMatte(run.outputs.first(), image, cancellation);
        result.backend = run.backend;
        result.adapter = run.adapter;
        result.fallbackReason = run.fallbackReason;
        result.milliseconds = timer.nsecsElapsed() / 1e6;
        return result;
    }
    AiSelectionResult object(const QImage &image, const QString &id, const QVector<QPointF> &points,
                             const QVector<int> &labels, std::shared_ptr<AiCancellation> cancellation) override {
        std::lock_guard lock(mutex_);
        check(cancellation);
        require(id == "sam2" || id == "mobilesam", "Unknown AI object model");
        require(!points.isEmpty() && points.size() <= 1024 && points.size() == labels.size() &&
                    labels.contains(1), "Add a foreground point first");
        for (qsizetype i = 0; i < points.size(); ++i)
            require(std::isfinite(points[i].x()) && std::isfinite(points[i].y()) &&
                        points[i].x() >= 0 && points[i].y() >= 0 &&
                        points[i].x() < image.width() && points[i].y() < image.height() &&
                        (labels[i] == 0 || labels[i] == 1), "Invalid AI object point");
        QElapsedTimer timer;
        timer.start();
        const auto kind = id == "sam2" ? AiImageKind::SAM2 : AiImageKind::MobileSAM;
        auto &sam = objects_[id];
        if (!sam) {
            const auto &model = verified(id, cancellation);
            sam = AiSamModel::open(aiModelPath(model, model.assets[0], root_),
                                   aiModelPath(model, model.assets[1], root_), kind, options_);
        }
        check(cancellation);
        auto encoded = sam->encodeOnce(image, cancellation);
        auto run = sam->decode(encoded, points, labels, {}, cancellation);
        auto result = aiObjectSelection(run, image, kind, cancellation);
        result.encoderRuns = sam->encoderRuns();
        result.milliseconds = timer.nsecsElapsed() / 1e6;
        return result;
    }
  private:
    const AiModelDefinition &verified(const QString &id, const std::shared_ptr<AiCancellation> &cancellation) {
        const auto &model = definition(id);
        const auto state = inspectAiModel(model, root_, cancellation);
        require(state.installed,
                "Import the required model using Help > AI Models before selecting. " + state.message);
        check(cancellation);
        return model;
    }
    QString root_;
    AiOptions options_;
    std::mutex mutex_;
    std::shared_ptr<AiSession> subject_;
    std::map<QString, std::shared_ptr<AiSamModel>> objects_;
};
}
std::shared_ptr<AiSelectionService> createAiSelectionService(QString root, AiOptions options) {
    return std::make_shared<SelectionService>(std::move(root), options);
}
static QImage subjectMask(const AiTensor &logits, const QImage &image,
                          std::shared_ptr<AiCancellation> cancellation, bool binary) {
    require(!image.isNull() && logits.type == AiTensorType::Float32 && logits.shape.size() == 4 &&
                logits.shape[0] == 1 && logits.shape[1] == 1 && logits.shape[2] > 0 && logits.shape[3] > 0,
            "Invalid subject mask contract");
    check(cancellation);
    auto probabilities = logits.floatValues();
    for (auto &value : probabilities) {
        require(std::isfinite(value), "Nonfinite subject mask");
        value = 1.0f / (1.0f + std::exp(-std::clamp(value, -80.0f, 80.0f)));
    }
    const int w = int(logits.shape[3]), h = int(logits.shape[2]);
    require(std::all_of(probabilities.cbegin(), probabilities.cend(), [](float v) { return std::isfinite(v); }),
            "Nonfinite subject mask");
    QImage mask(image.size(), QImage::Format_Grayscale8);
    require(!mask.isNull(), "Not enough memory for AI selection");
    for (int y = 0; y < mask.height(); ++y) {
        check(cancellation);
        auto row = mask.scanLine(y);
        const double sy = std::clamp((y + .5) * h / mask.height() - .5, 0.0, double(h - 1));
        const int y0 = int(sy), y1 = std::min(y0 + 1, h - 1);
        for (int x = 0; x < mask.width(); ++x) {
            const double sx = std::clamp((x + .5) * w / mask.width() - .5, 0.0, double(w - 1));
            const int x0 = int(sx), x1 = std::min(x0 + 1, w - 1);
            const auto a = probabilities[y0 * w + x0] * (1 - (sx - x0)) + probabilities[y0 * w + x1] * (sx - x0);
            const auto b = probabilities[y1 * w + x0] * (1 - (sx - x0)) + probabilities[y1 * w + x1] * (sx - x0);
            const auto value = a * (1 - (sy - y0)) + b * (sy - y0);
            row[x] = !image.pixelColor(x, y).alpha() ? 0
                     : binary ? (value > .5 ? 255 : 0)
                              : uchar(std::clamp(std::lround(value * 255), 0L, 255L));
        }
    }
    check(cancellation);
    return mask;
}
QImage aiSubjectSelection(const AiTensor &logits, const QImage &image,
                          std::shared_ptr<AiCancellation> cancellation) {
    return subjectMask(logits, image, cancellation, true);
}
QImage aiSubjectMatte(const AiTensor &logits, const QImage &image,
                      std::shared_ptr<AiCancellation> cancellation) {
    return subjectMask(logits, image, cancellation, false);
}
AiSelectionResult aiObjectSelection(const AiRunResult &run, const QImage &image, AiImageKind kind,
                                    std::shared_ptr<AiCancellation> cancellation) {
    require(!image.isNull() && kind != AiImageKind::BiRefNet && run.outputs.size() == 3,
            "Invalid object mask contract");
    const auto &masks = run.outputs[0], &scoreTensor = run.outputs[1], &low = run.outputs[2];
    const int count = kind == AiImageKind::MobileSAM ? 4 : 3;
    require(masks.name == "masks" && masks.shape == QVector<qint64>{1, count, image.height(), image.width()} &&
                scoreTensor.name == "iou_predictions" && scoreTensor.shape == QVector<qint64>{1, count} &&
                low.name == "low_res_masks" && low.shape == QVector<qint64>{1, count, 256, 256},
            "SAM returned an unexpected selection contract");
    const auto scores = scoreTensor.floatValues();
    require(std::all_of(scores.cbegin(), scores.cend(), [](float v) { return std::isfinite(v); }), "Nonfinite SAM score");
    const int first = kind == AiImageKind::MobileSAM ? 1 : 0;
    const int chosen = int(std::max_element(scores.cbegin() + first, scores.cend()) - scores.cbegin());
    const auto values = masks.floatValues();
    const qsizetype offset = qsizetype(chosen) * image.width() * image.height();
    QImage mask(image.size(), QImage::Format_Grayscale8);
    require(!mask.isNull(), "Not enough memory for AI selection");
    for (int y = 0; y < mask.height(); ++y) {
        check(cancellation);
        auto row = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            const auto value = values[offset + qsizetype(y) * mask.width() + x];
            require(std::isfinite(value), "Nonfinite object mask");
            row[x] = value > 0 && image.pixelColor(x, y).alpha() ? 255 : 0;
        }
    }
    check(cancellation);
    return {mask, run.backend, run.adapter, run.fallbackReason, run.milliseconds, chosen};
}
QImage combineAiSelection(const QImage &original, const QImage &mask, int mode) {
    require(mask.format() == QImage::Format_Grayscale8 && mode >= 0 && mode <= 3 &&
                (original.isNull() || (original.size() == mask.size() && original.format() == mask.format())),
            "Invalid AI selection combination");
    if (original.isNull() || mode == 0)
        return mask;
    auto result = mask;
    result.detach();
    for (int y = 0; y < result.height(); ++y) {
        auto row = result.scanLine(y);
        const auto old = original.constScanLine(y);
        for (int x = 0; x < result.width(); ++x)
            row[x] = mode == 1 ? std::max(row[x], old[x])
                     : mode == 2 ? uchar((int(old[x]) * (255 - row[x]) + 127) / 255)
                                 : uchar((int(old[x]) * row[x] + 127) / 255);
    }
    return result;
}
} // namespace compositor
