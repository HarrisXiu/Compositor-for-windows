#include "ai_session.h"
#include "ai_preprocess.h"
#include "ai_sam.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

using namespace compositor;
class AiTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory_;
    QString model_;
  private slots:
    void initTestCase() {
        QVERIFY(directory_.isValid());
        model_ = directory_.filePath(QString::fromUtf8("模型.onnx"));
        QFile file(model_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = QByteArray::fromBase64("CAk6mAEKFwoBeAoFc2NhbGUSBnNjYWxlZCIDTXVsChYKBnNjYWxlZAoEYmlhcxIBeSIDQWRkEgphaV9maXh0dXJlKg8QAUIFc2NhbGVKBAAAAEAqDhABQgRiaWFzSgQAAIA/WhsKAXgSFgoUCAESEAoCCAEKAggDCgIIAgoCCAJiGwoBeRIWChQIARIQCgIIAQoCCAMKAggCCgIIAkIECgAQEQ==");
        QCOMPARE(file.write(bytes), bytes.size());
    }
    void defaultThreadBudget() {
        QVERIFY(aiDefaultThreads() >= 1 && aiDefaultThreads() <= 12);
        QCOMPARE(AiOptions().threads, aiDefaultThreads());
    }
    void syntheticCpuInferenceAndUnicodePath() {
        AiOptions options;
        options.policy = AiProviderPolicy::CpuOnly;
        auto session = AiSession::open(model_, options);
        QVector<float> values(12);
        for (int i = 0; i < values.size(); ++i)
            values[i] = float(i);
        auto result = session->run({AiTensor::floats("x", {1, 3, 2, 2}, values)});
        QCOMPARE(result.backend, QString("CPU"));
        QCOMPARE(result.outputs.size(), 1);
        auto output = result.outputs.first().floatValues();
        QCOMPARE(output.size(), values.size());
        for (int i = 0; i < output.size(); ++i)
            QCOMPARE(output[i], values[i] * 2 + 1);
    }
    void invalidDirectMLAdapterFallsBack() {
        AiOptions options;
        options.adapterIndex = 9999;
        auto session = AiSession::open(model_, options);
        auto result = session->run({AiTensor::floats("x", {1, 3, 2, 2}, QVector<float>(12, 3))});
        QCOMPARE(result.backend, QString("CPU"));
        QVERIFY(!result.fallbackReason.isEmpty());
        QCOMPARE(result.outputs.first().floatValues().first(), 7.0f);
    }
    void asynchronousInference() {
        AiOptions options;
        options.policy = AiProviderPolicy::CpuOnly;
        auto loading = AiSession::openAsync(model_, options);
        loading.waitForFinished();
        auto session = loading.result();
        auto job = session->runAsync({AiTensor::floats("x", {1, 3, 2, 2}, QVector<float>(12, 5))});
        job.waitForFinished();
        QCOMPARE(job.result().outputs.first().floatValues().first(), 11.0f);
    }
    void cancellationAndInvalidBindings() {
        AiOptions options;
        options.policy = AiProviderPolicy::CpuOnly;
        auto session = AiSession::open(model_, options);
        auto cancellation = std::make_shared<AiCancellation>();
        cancellation->cancel();
        const QList<AiTensor> inputs{AiTensor::floats("x", {1, 3, 2, 2}, QVector<float>(12))};
        QVERIFY_EXCEPTION_THROWN(session->run(inputs, cancellation), AiCancelled);
        QVERIFY_EXCEPTION_THROWN(session->run({AiTensor::floats("wrong", {12}, QVector<float>(12))}), std::exception);
        QVERIFY_EXCEPTION_THROWN(session->run({AiTensor::floats("x", {1, 3, 2, 1}, QVector<float>(6))}), std::exception);
    }
    void cancellationInterruptsActiveInference() {
        const auto path = directory_.filePath("cancel.onnx");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = QByteArray::fromBase64("CAk6zAIK+AEKBWNvdW50Cgljb25kaXRpb24KAXgSAXkiBExvb3Aq1wEKBGJvZHkyywEKJQoJY29uZGl0aW9uEg5uZXh0X2NvbmRpdGlvbiIISWRlbnRpdHkKHQoFc3RhdGUKA29uZRIKbmV4dF9zdGF0ZSIDQWRkEglsb29wX2JvZHkqDRABQgNvbmVKBAAAgD9aEwoJaXRlcmF0aW9uEgYKBAgHEgBaEwoJY29uZGl0aW9uEgYKBAgJEgBaDwoFc3RhdGUSBgoECAESAGIYCg5uZXh0X2NvbmRpdGlvbhIGCgQICRIAYhQKCm5leHRfc3RhdGUSBgoECAESAKABBRIOY2FuY2VsX2ZpeHR1cmUqExAHQgVjb3VudEoIAMqaOwAAAAAqEBAJQgljb25kaXRpb25KAQFaCwoBeBIGCgQIARIAYgsKAXkSBgoECAESAEIECgAQEQ==");
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        AiOptions options;
        options.policy = AiProviderPolicy::CpuOnly;
        auto session = AiSession::open(path, options);
        auto cancellation = std::make_shared<AiCancellation>();
        auto job = session->runAsync({AiTensor::floats("x", {}, {0})}, cancellation);
        QTRY_VERIFY_WITH_TIMEOUT(job.isStarted(), 1000);
        QTest::qWait(100);
        QVERIFY(!job.isFinished());
        cancellation->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(job.isFinished(), 5000);
        QVERIFY_EXCEPTION_THROWN(job.result(), AiCancelled);
    }
    void samReusesEncodedImageBetweenClicks() {
        const auto encoder = directory_.filePath("encoder.onnx"), decoder = directory_.filePath("decoder.onnx");
        const auto encoderBytes = QByteArray::fromBase64("CAk6yAEKNAoPZW1iZWRkaW5nX3NoYXBlEhBpbWFnZV9lbWJlZGRpbmdzIg9Db25zdGFudE9mU2hhcGUSB2VuY29kZXIqNwgEEAdCD2VtYmVkZGluZ19zaGFwZUogAQAAAAAAAAAAAQAAAAAAAEAAAAAAAAAAQAAAAAAAAABaIQoFaW1hZ2USGAoWCAESEgoCCAEKAggDCgMIgAgKAwiACGIrChBpbWFnZV9lbWJlZGRpbmdzEhcKFQgBEhEKAggBCgMIgAIKAghACgIIQEIECgAQEQ==");
        const auto decoderBytes = QByteArray::fromBase64("CAk6tgUKJQoMb3JpZ19pbV9zaXplEgRzaXplIgRDYXN0KgkKAnRvGAegAQIKLwoGcHJlZml4CgRzaXplEgptYXNrX3NoYXBlIgZDb25jYXQqCwoEYXhpcxgAoAECCiQKCm1hc2tfc2hhcGUSBW1hc2tzIg9Db25zdGFudE9mU2hhcGUKLwoLc2NvcmVfc2hhcGUSD2lvdV9wcmVkaWN0aW9ucyIPQ29uc3RhbnRPZlNoYXBlCisKCWxvd19zaGFwZRINbG93X3Jlc19tYXNrcyIPQ29uc3RhbnRPZlNoYXBlEgdkZWNvZGVyKh4IAhAHQgZwcmVmaXhKEAEAAAAAAAAABAAAAAAAAAAqIwgCEAdCC3Njb3JlX3NoYXBlShABAAAAAAAAAAQAAAAAAAAAKjEIBBAHQglsb3dfc2hhcGVKIAEAAAAAAAAABAAAAAAAAAAAAQAAAAAAAAABAAAAAAAAWisKEGltYWdlX2VtYmVkZGluZ3MSFwoVCAESEQoCCAEKAwiAAgoCCEAKAghAWigKDHBvaW50X2Nvb3JkcxIYChYIARISCgIIAQoIEgZwb2ludHMKAggCWiQKDHBvaW50X2xhYmVscxIUChIIARIOCgIIAQoIEgZwb2ludHNaJgoKbWFza19pbnB1dBIYChYIARISCgIIAQoCCAEKAwiAAgoDCIACWhwKDmhhc19tYXNrX2lucHV0EgoKCAgBEgQKAggBWhoKDG9yaWdfaW1fc2l6ZRIKCggIARIECgIIAmIqCgVtYXNrcxIhCh8IARIbCgIIAQoCCAQKCBIGaGVpZ2h0CgcSBXdpZHRoYiEKD2lvdV9wcmVkaWN0aW9ucxIOCgwIARIICgIIAQoCCARiKQoNbG93X3Jlc19tYXNrcxIYChYIARISCgIIAQoCCAQKAwiAAgoDCIACQgQKABAR");
        for (const auto &item : QList<QPair<QString, QByteArray>>{{encoder, encoderBytes}, {decoder, decoderBytes}}) {
            QFile output(item.first);
            QVERIFY(output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write(item.second), item.second.size());
        }
        AiOptions options;
        options.policy = AiProviderPolicy::CpuOnly;
        auto model = AiSamModel::open(encoder, decoder, AiImageKind::MobileSAM, options);
        QImage image(8, 6, QImage::Format_RGB888);
        image.fill(Qt::green);
        const auto encoded = model->encodeOnce(image);
        QCOMPARE(model->encoderRuns(), quint64(1));
        QVERIFY(encoded == model->encodeOnce(image));
        for (auto point : {QPointF(2, 2), QPointF(6, 4)}) {
            const auto result = model->decode(encoded, {point}, {1});
            QCOMPARE(result.outputs[0].shape, (QVector<qint64>{1, 4, 6, 8}));
        }
        QCOMPARE(model->encoderRuns(), quint64(1));
        image.setPixelColor(0, 0, Qt::red);
        model->encodeOnce(image);
        QCOMPARE(model->encoderRuns(), quint64(2));
    }
    void pillowResizeAndNormalization() {
        QImage image(3, 2, QImage::Format_RGB888);
        const int values[]{255,0,0,0,255,0,0,0,255,0,0,0,255,255,255,127,127,127};
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 3; ++x)
                image.setPixelColor(x, y, QColor(values[(y * 3 + x) * 3], values[(y * 3 + x) * 3 + 1], values[(y * 3 + x) * 3 + 2]));
        auto prepared = prepareAiImage(image, AiImageKind::BiRefNet, {}, 2);
        const int expected[]{159,96,0,0,96,159,96,96,96,175,175,175};
        const float mean[]{.485f, .456f, .406f}, deviation[]{.229f, .224f, .225f};
        auto actual = prepared.tensor.floatValues();
        for (int pixel = 0; pixel < 4; ++pixel)
            for (int channel = 0; channel < 3; ++channel)
                QVERIFY(std::abs(actual[channel * 4 + pixel] - (expected[pixel * 3 + channel] / 255.0f - mean[channel]) / deviation[channel]) < 1e-6f);
    }
    void mobilePaddingAndPointConversion() {
        QImage image(2, 1, QImage::Format_RGB888);
        image.fill(Qt::red);
        auto prepared = prepareAiImage(image, AiImageKind::MobileSAM, {}, 4);
        QCOMPARE(prepared.originalSize, QSize(2, 1));
        QCOMPARE(prepared.resizedSize, QSize(4, 2));
        auto values = prepared.tensor.floatValues();
        QVERIFY(std::abs(values.first() - (255.f - 123.675f) / 58.395f) < 1e-6f);
        for (int channel = 0; channel < 3; ++channel)
            for (int pixel = 8; pixel < 16; ++pixel)
                QCOMPARE(values[channel * 16 + pixel], 0.0f);
        QCOMPARE(convertAiPoint({1, .5}, prepared, AiImageKind::MobileSAM, 4), QPointF(2, 1));
        QCOMPARE(convertAiPoint({1, .5}, prepared, AiImageKind::SAM2, 4), QPointF(2, 2));
    }
};
QTEST_MAIN(AiTests)
#include "ai_tests.moc"
