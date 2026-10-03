#include "ai_sam.h"
#include "document.h"
#include <QtConcurrent/QtConcurrentRun>

namespace compositor {
std::shared_ptr<AiSamModel> AiSamModel::open(QString encoder, QString decoder, AiImageKind kind,
                                          const AiOptions &options) {
    require(kind != AiImageKind::BiRefNet, "BiRefNet is not a prompted SAM model");
    auto model = std::shared_ptr<AiSamModel>(new AiSamModel);
    model->kind_ = kind;
    auto encoderOptions = options, decoderOptions = options;
    if (!options.profilePrefix.isEmpty()) {
        encoderOptions.profilePrefix += "-encoder";
        decoderOptions.profilePrefix += "-decoder";
    }
    model->encoder_ = AiSession::open(encoder, encoderOptions);
    model->decoder_ = AiSession::open(decoder, decoderOptions);
    return model;
}
QFuture<std::shared_ptr<AiSamModel>> AiSamModel::openAsync(QString encoder, QString decoder,
                                                       AiImageKind kind, AiOptions options) {
    return QtConcurrent::run([encoder = std::move(encoder), decoder = std::move(decoder), kind, options] {
        return open(encoder, decoder, kind, options);
    });
}
std::shared_ptr<const AiEncodedImage> AiSamModel::encodeOnce(const QImage &image,
                                                          std::shared_ptr<AiCancellation> cancellation) {
    if (!cancellation)
        cancellation = std::make_shared<AiCancellation>();
    cancellation->check();
    std::lock_guard lock(mutex_);
    cancellation->check();
    if (cached_ && cached_->imageKey == image.cacheKey() && cached_->geometry.originalSize == image.size())
        return cached_;
    auto encoded = std::make_shared<AiEncodedImage>();
    encoded->imageKey = image.cacheKey();
    encoded->geometry = prepareAiImage(image, kind_, cancellation);
    encoded->encoderResult = encoder_->run({encoded->geometry.tensor}, cancellation);
    encoded->features = encoded->encoderResult.outputs;
    cancellation->check();
    ++encoderRuns_;
    cached_ = encoded;
    return encoded;
}
QFuture<std::shared_ptr<const AiEncodedImage>> AiSamModel::encodeAsync(QImage image,
                                                                  std::shared_ptr<AiCancellation> cancellation) {
    return QtConcurrent::run([self = shared_from_this(), image = std::move(image), cancellation] {
        return self->encodeOnce(image, cancellation);
    });
}
AiRunResult AiSamModel::decode(const std::shared_ptr<const AiEncodedImage> &image,
                             const QVector<QPointF> &points, const QVector<int> &labels,
                             AiTensor previousMask, std::shared_ptr<AiCancellation> cancellation) {
    if (!cancellation)
        cancellation = std::make_shared<AiCancellation>();
    cancellation->check();
    require(image && !points.isEmpty() && points.size() == labels.size() && points.size() <= 1024,
            "SAM requires matching point coordinates and labels");
    const auto size = image->geometry.originalSize;
    const bool mobile = kind_ == AiImageKind::MobileSAM;
    require(qint64(size.width()) * size.height() <= MaxAiTensorElements / (mobile ? 4 : 3),
            "SAM output exceeds its memory budget");
    QVector<float> coordinates;
    QVector<qint32> integerLabels;
    QVector<float> floatLabels;
    for (qsizetype i = 0; i < points.size(); ++i) {
        require(labels[i] == 0 || labels[i] == 1, "SAM point labels must be foreground or background");
        const auto point = convertAiPoint(points[i], image->geometry, kind_);
        coordinates << float(point.x()) << float(point.y());
        integerLabels << labels[i];
        floatLabels << float(labels[i]);
    }
    if (mobile) {
        coordinates << 0.0f << 0.0f;
        floatLabels << -1.0f;
    }
    auto inputs = image->features;
    inputs << AiTensor::floats("point_coords", {1, mobile ? floatLabels.size() : integerLabels.size(), 2}, coordinates);
    inputs << (mobile ? AiTensor::floats("point_labels", {1, floatLabels.size()}, floatLabels)
                     : AiTensor::integers("point_labels", {1, integerLabels.size()}, integerLabels));
    const bool hasMask = !previousMask.bytes.isEmpty();
    if (hasMask) {
        previousMask.validate();
        require(previousMask.type == AiTensorType::Float32 && previousMask.shape == QVector<qint64>{1, 1, 256, 256},
                "SAM feedback must be one 256x256 float32 logit mask");
        previousMask.name = mobile ? "mask_input" : "input_masks";
    } else {
        previousMask = AiTensor::floats(mobile ? "mask_input" : "input_masks", {1, 1, 256, 256}, QVector<float>(256 * 256, 0));
    }
    inputs << previousMask;
    inputs << AiTensor::floats(mobile ? "has_mask_input" : "has_input_masks", {1}, {hasMask ? 1.0f : 0.0f});
    inputs << (mobile ? AiTensor::floats("orig_im_size", {2}, {float(size.height()), float(size.width())})
                     : AiTensor::sizes("original_image_size", {2}, {size.height(), size.width()}));
    auto result = decoder_->run(inputs, cancellation);
    require(result.outputs.size() == 3 && result.outputs[0].name == "masks" &&
            result.outputs[0].shape == QVector<qint64>{1, mobile ? 4 : 3, size.height(), size.width()},
            "SAM returned an unexpected mask contract");
    return result;
}
QFuture<AiRunResult> AiSamModel::decodeAsync(std::shared_ptr<const AiEncodedImage> image,
                                          QVector<QPointF> points, QVector<int> labels,
                                          AiTensor previousMask, std::shared_ptr<AiCancellation> cancellation) {
    return QtConcurrent::run([self = shared_from_this(), image = std::move(image), points = std::move(points),
                              labels = std::move(labels), previousMask = std::move(previousMask), cancellation] {
        return self->decode(image, points, labels, previousMask, cancellation);
    });
}
quint64 AiSamModel::encoderRuns() const {
    std::lock_guard lock(mutex_);
    return encoderRuns_;
}
void AiSamModel::clearCache() {
    std::lock_guard lock(mutex_);
    cached_.reset();
}
std::shared_ptr<AiSession> AiSamModel::encoderSession() const { return encoder_; }
std::shared_ptr<AiSession> AiSamModel::decoderSession() const { return decoder_; }
}
