#include "ai_preprocess.h"
#include "document.h"
#include <QColorSpace>
#include <algorithm>
#include <cmath>
#include <vector>

namespace compositor {
namespace {
struct Weights {
    int first = 0;
    std::vector<float> values;
    std::vector<qint32> fixed;
};
std::vector<Weights> bilinearWeights(int source, int target) {
    constexpr double precision = 1 << 22;
    const double scale = double(source) / target, support = std::max(1.0, scale);
    std::vector<Weights> result(static_cast<size_t>(target));
    for (int position = 0; position < target; ++position) {
        const double center = (position + .5) * scale;
        auto &weights = result[size_t(position)];
        weights.first = std::max(0, int(center - support + .5));
        const int end = std::min(source, int(center + support + .5));
        std::vector<double> raw;
        double total = 0;
        for (int x = weights.first; x < end; ++x) {
            const double value = std::max(0.0, 1 - std::abs((x + .5 - center) / support));
            raw.push_back(value);
            total += value;
        }
        require(total > 0, "Invalid AI resampling weights");
        for (double value : raw) {
            weights.values.push_back(float(value / total));
            weights.fixed.push_back(qint32(value / total * precision + .5));
        }
    }
    return result;
}
void check(const std::shared_ptr<AiCancellation> &cancellation) {
    if (cancellation)
        cancellation->check();
}
QImage resizePillow(const QImage &source, QSize target, const std::shared_ptr<AiCancellation> &cancellation) {
    const auto horizontal = bilinearWeights(source.width(), target.width());
    const auto vertical = bilinearWeights(source.height(), target.height());
    QImage temporary(target.width(), source.height(), QImage::Format_RGB888);
    QImage result(target, QImage::Format_RGB888);
    require(!temporary.isNull() && !result.isNull(), "Not enough memory for AI image resampling");
    for (int y = 0; y < source.height(); ++y) {
        check(cancellation);
        const auto *input = source.constScanLine(y);
        auto *output = temporary.scanLine(y);
        for (int x = 0; x < target.width(); ++x) {
            const auto &weights = horizontal[size_t(x)];
            for (int channel = 0; channel < 3; ++channel) {
                qint64 value = 1 << 21;
                for (size_t index = 0; index < weights.fixed.size(); ++index)
                    value += qint64(input[(weights.first + int(index)) * 3 + channel]) * weights.fixed[index];
                output[x * 3 + channel] = uchar(std::clamp<qint64>(value >> 22, 0, 255));
            }
        }
    }
    for (int y = 0; y < target.height(); ++y) {
        check(cancellation);
        const auto &weights = vertical[size_t(y)];
        auto *output = result.scanLine(y);
        for (int x = 0; x < target.width() * 3; ++x) {
            qint64 value = 1 << 21;
            for (size_t index = 0; index < weights.fixed.size(); ++index)
                value += qint64(temporary.constScanLine(weights.first + int(index))[x]) * weights.fixed[index];
            output[x] = uchar(std::clamp<qint64>(value >> 22, 0, 255));
        }
    }
    return result;
}
QVector<float> resizeTensor(const QImage &source, int side, const std::shared_ptr<AiCancellation> &cancellation) {
    const auto horizontal = bilinearWeights(source.width(), side);
    const auto vertical = bilinearWeights(source.height(), side);
    QVector<float> temporary(qsizetype(source.height()) * side * 3);
    QVector<float> result(qsizetype(side) * side * 3);
    for (int y = 0; y < source.height(); ++y) {
        check(cancellation);
        const auto *input = source.constScanLine(y);
        for (int x = 0; x < side; ++x) {
            const auto &weights = horizontal[size_t(x)];
            for (int channel = 0; channel < 3; ++channel) {
                float value = 0;
                for (size_t index = 0; index < weights.values.size(); ++index)
                    value += (input[(weights.first + int(index)) * 3 + channel] / 255.0f) * weights.values[index];
                temporary[(qsizetype(y) * side + x) * 3 + channel] = value;
            }
        }
    }
    for (int y = 0; y < side; ++y) {
        check(cancellation);
        const auto &weights = vertical[size_t(y)];
        for (int x = 0; x < side * 3; ++x) {
            float value = 0;
            for (size_t index = 0; index < weights.values.size(); ++index)
                value += temporary[qsizetype(weights.first + int(index)) * side * 3 + x] * weights.values[index];
            result[qsizetype(y) * side * 3 + x] = value;
        }
    }
    return result;
}
}
AiPreparedImage prepareAiImage(const QImage &source, AiImageKind kind,
                              std::shared_ptr<AiCancellation> cancellation, int side) {
    require(!source.isNull() && side > 0 && side <= 2048 && qint64(source.width()) * source.height() <= MaxSurfacePixels,
            "Image exceeds AI preprocessing limits");
    check(cancellation);
    auto image = source;
    if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace(QColorSpace::SRgb))
        image = image.convertedToColorSpace(QColorSpace::SRgb);
    image = image.convertToFormat(QImage::Format_RGB888);
    check(cancellation);
    const QSize original = image.size();
    QSize resized(side, side);
    if (kind == AiImageKind::MobileSAM) {
        const double scale = double(side) / std::max(original.width(), original.height());
        resized = {std::max(1, int(original.width() * scale + .5)), std::max(1, int(original.height() * scale + .5))};
    }
    QVector<float> values(qsizetype(side) * side * 3, 0.0f);
    const float mean[]{.485f, .456f, .406f}, deviation[]{.229f, .224f, .225f};
    const float mobileMean[]{123.675f, 116.28f, 103.53f}, mobileDeviation[]{58.395f, 57.12f, 57.375f};
    if (kind == AiImageKind::SAM2) {
        const auto pixels = resizeTensor(image, side, cancellation);
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
                for (int channel = 0; channel < 3; ++channel)
                    values[qsizetype(channel) * side * side + y * side + x] =
                        (pixels[(qsizetype(y) * side + x) * 3 + channel] - mean[channel]) / deviation[channel];
    } else {
        const auto pixels = resizePillow(image, resized, cancellation);
        for (int y = 0; y < resized.height(); ++y) {
            check(cancellation);
            const auto *row = pixels.constScanLine(y);
            for (int x = 0; x < resized.width(); ++x)
                for (int channel = 0; channel < 3; ++channel) {
                    const float value = row[x * 3 + channel];
                    values[qsizetype(channel) * side * side + y * side + x] = kind == AiImageKind::MobileSAM
                        ? (value - mobileMean[channel]) / mobileDeviation[channel]
                        : (value / 255.0f - mean[channel]) / deviation[channel];
                }
        }
    }
    check(cancellation);
    return {AiTensor::floats("image", {1, 3, side, side}, values), original, resized};
}
QPointF convertAiPoint(QPointF point, const AiPreparedImage &image, AiImageKind kind, int side) {
    require(image.originalSize.width() > 0 && image.originalSize.height() > 0 &&
            std::isfinite(point.x()) && std::isfinite(point.y()), "Invalid AI point coordinates");
    if (kind == AiImageKind::MobileSAM)
        return {float(point.x() * image.resizedSize.width() / image.originalSize.width()),
                float(point.y() * image.resizedSize.height() / image.originalSize.height())};
    return {float(point.x()) * float(double(side) / image.originalSize.width()),
            float(point.y()) * float(double(side) / image.originalSize.height())};
}
QVector<float> aiProbabilities(const AiTensor &logits) {
    auto values = logits.floatValues();
    for (auto &value : values)
        value = 1.0f / (1.0f + std::exp(-std::clamp(value, -80.0f, 80.0f)));
    return values;
}
}
