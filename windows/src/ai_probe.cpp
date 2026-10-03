#include "ai_sam.h"
#include "ai_models.h"
#include <QSslSocket>
#include "document.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace compositor;
namespace {
QByteArray readFile(const QString &path, qint64 maximum) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= maximum, "Missing or oversized verification file: " + path);
    return file.readAll();
}
QByteArray digest(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read model for verification: " + path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "Cannot hash model");
    return hash.result().toHex();
}
AiTensor reference(const QDir &directory, const QString &name, const QJsonObject &record) {
    QVector<qint64> shape;
    for (auto dimension : record.value("shape").toArray())
        shape.push_back(qint64(dimension.toDouble()));
    const auto typeName = record.value("type").toString();
    require(typeName == "float32" || typeName == "int32" || typeName == "int64", "Unsupported reference tensor type");
    AiTensor tensor{name, shape, typeName == "float32" ? AiTensorType::Float32 : typeName == "int32" ? AiTensorType::Int32 : AiTensorType::Int64, {}};
    const auto expected = tensor.elements() * (tensor.type == AiTensorType::Int64 ? 8 : 4);
    const auto filename = record.value("file").toString();
    require(QFileInfo(filename).fileName() == filename, "Unsafe reference filename");
    const auto compressed = readFile(directory.filePath(filename), 512LL * 1024 * 1024);
    require(compressed.size() >= 4 && qFromBigEndian<quint32>(compressed.constData()) == quint64(expected), "Invalid reference data size");
    tensor.bytes = qUncompress(compressed);
    tensor.validate();
    require(QCryptographicHash::hash(tensor.bytes, QCryptographicHash::Sha256).toHex() == record.value("sha256").toString().toUtf8(), "Reference SHA256 mismatch");
    return tensor;
}
QJsonObject compare(const AiTensor &actual, const AiTensor &expected, bool mask = false,
                    double atol = .005, double mismatchLimit = .01, double rtol = .0001) {
    if (actual.shape != expected.shape || actual.type != expected.type)
        return {{"passed", false}, {"reason", "Tensor type or shape differs"}};
    auto left = actual.floatValues(), right = expected.floatValues();
    double largest = 0, sum = 0;
    qint64 mismatches = 0;
    for (qsizetype i = 0; i < left.size(); ++i) {
        if (!std::isfinite(left[i]) || !std::isfinite(right[i]))
            return {{"passed", false}, {"reason", "Nonfinite tensor"}};
        const double difference = std::abs(double(left[i]) - right[i]);
        largest = std::max(largest, difference);
        sum += difference;
        mismatches += difference > atol + rtol * std::max(std::abs(double(left[i])), std::abs(double(right[i])));
    }
    const double fraction = double(mismatches) / left.size();
    QJsonObject result{{"passed", fraction <= mismatchLimit}, {"max_abs_error", largest},
                       {"mean_abs_error", sum / left.size()}, {"mismatch_fraction", fraction}};
    if (mask) {
        const qint64 pixels = actual.shape[actual.shape.size() - 1] * actual.shape[actual.shape.size() - 2];
        double minimumIoU = 1, maximumMean = 0;
        for (qint64 offset = 0; offset < left.size(); offset += pixels) {
            qint64 intersection = 0, unionCount = 0;
            double errors = 0;
            for (qint64 i = offset; i < offset + pixels; ++i) {
                const bool foregroundLeft = left[i] > 0, foregroundRight = right[i] > 0;
                intersection += foregroundLeft && foregroundRight;
                unionCount += foregroundLeft || foregroundRight;
                const double a = 1 / (1 + std::exp(-std::clamp(double(left[i]), -80.0, 80.0)));
                const double b = 1 / (1 + std::exp(-std::clamp(double(right[i]), -80.0, 80.0)));
                errors += std::abs(a - b);
            }
            minimumIoU = std::min(minimumIoU, unionCount ? double(intersection) / unionCount : 1.0);
            maximumMean = std::max(maximumMean, errors / pixels);
        }
        result["min_mask_iou"] = minimumIoU;
        result["max_mask_probability_mae"] = maximumMean;
        result["passed"] = result.value("passed").toBool() && minimumIoU >= .995 && maximumMean <= .001;
    }
    return result;
}
void check(QJsonArray &checks, QString name, QJsonObject result) {
    result["name"] = name;
    checks.append(result);
    if (!result.value("passed").toBool())
        std::fprintf(stderr, "Failed: %s\n", name.toUtf8().constData());
}
void compareOutputs(QJsonArray &checks, const QDir &directory, const QString &prefix,
                    const QList<AiTensor> &actual, const QJsonObject &expected, bool decoder) {
    require(actual.size() == expected.size(), "Reference output count differs from model");
    for (const auto &tensor : actual) {
        require(expected.contains(tensor.name), "Reference output name differs from model");
        check(checks, prefix + "/" + tensor.name,
              compare(tensor, reference(directory, tensor.name, expected.value(tensor.name).toObject()),
                      tensor.name == "masks" || tensor.name == "logits", .005, decoder ? .001 : .01));
    }
}
QJsonObject timing(const AiRunResult &result) {
    return {{"backend", result.backend}, {"adapter", result.adapter}, {"fallback_reason", result.fallbackReason}, {"run_ms", result.milliseconds}, {"metacommands_disabled", result.metacommandsDisabled}};
}
QJsonObject profile(const QString &path) {
    if (path.isEmpty()) return {};
    const auto events = QJsonDocument::fromJson(readFile(path, 512LL * 1024 * 1024)).array();
    QJsonObject providers;
    for (const auto &value : events) {
        const auto arguments = value.toObject().value("args").toObject();
        const auto provider = arguments.value("provider").toString();
        if (!provider.isEmpty())
            providers[provider] = providers.value(provider).toInt() + 1;
    }
    return {{"file", path}, {"executed_nodes", providers}};
}
AiImageKind kind(const QString &value) {
    if (value == "sam2") return AiImageKind::SAM2;
    if (value == "mobilesam") return AiImageKind::MobileSAM;
    require(value == "birefnet", "Unknown reference model kind");
    return AiImageKind::BiRefNet;
}
}
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"references", "Local Python reference manifest", "file"});
    parser.addOption({"provider", "cpu, prefer or dml (dml never silently falls back)", "policy", "cpu"});
    parser.addOption({"adapter", "DXGI adapter index; -1 selects a compatible high-performance GPU", "index", "-1"});
    parser.addOption({"output", "JSON verification report", "file"});
    parser.addOption({"write-model-licenses", "Write bundled licenses/notices for each catalog model", "directory"});
    parser.addOption({"catalog", "Write the built-in release catalog as JSON", "file"});
    parser.addOption({"runtime-check", "Check packaged runtime and HTTPS TLS support"});
    parser.addOption({"model", "Verify only this model ID from the manifest", "id"});
    parser.addOption({"image", "Verify only this reference image filename", "file"});
    parser.addOption({"reverse-images", "Reverse the reference image order for reuse diagnostics"});
    parser.addOption({"no-profile", "Disable profiling for CPU timing only"});
    parser.addOption({"repeat", "Repeat BiRefNet timing runs per image", "count", "1"});
    parser.addOption({"threads", "CPU intra-op thread count", "count", QString::number(aiDefaultThreads())});
    parser.addOption({"vendor-metacommands", "Diagnostic override of the Intel compatibility default"});
    parser.addOption({"disable-metacommands", "Use portable DirectML kernels for driver diagnostics"});
    parser.addOption({"list-adapters", "List DXGI adapters without running models"});
    parser.process(application);
    if (parser.isSet("write-model-licenses")) {
        try {
            for (const auto &model : aiModelCatalog())
                saveAiModelLicenses(model, parser.value("write-model-licenses"));
            return 0;
        } catch (const std::exception &error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
    if (parser.isSet("catalog")) {
        QFile file(parser.value("catalog"));
        const auto bytes = aiModelCatalogJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return 1;
        return 0;
    }
    if (parser.isSet("runtime-check")) {
        const bool tls = QSslSocket::supportsSsl();
        std::puts(QJsonDocument(QJsonObject{{"onnxruntime", aiRuntimeVersion()}, {"https_tls", tls},
                                           {"tls_backend", QSslSocket::activeBackend()}}).toJson().constData());
        return tls ? 0 : 1;
    }
    QJsonArray devices;
    for (const auto &adapter : aiAdapters())
        devices.append(QJsonObject{{"index", adapter.index}, {"name", adapter.name}, {"vendor", int(adapter.vendor)},
                                  {"dedicated_memory", double(adapter.dedicatedMemory)}, {"directx12", adapter.directX12}, {"software", adapter.software}});
    if (parser.isSet("list-adapters")) {
        std::puts(QJsonDocument(devices).toJson().constData());
        return 0;
    }
    QJsonObject report{{"status", "incomplete"}, {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                       {"os", QSysInfo::prettyProductName()}, {"native_onnxruntime", aiRuntimeVersion()}, {"adapters", devices}, {"requested_provider", parser.value("provider")},
                       {"quality_scope", "ONNX numerical fidelity, not ground-truth segmentation quality"}};
    QJsonArray checks, models;
    try {
        require(parser.isSet("references") && parser.isSet("output"), "--references and --output are required");
        const QFileInfo manifest(parser.value("references"));
        const QDir directory(manifest.absolutePath());
        const auto data = QJsonDocument::fromJson(readFile(manifest.filePath(), 4 * 1024 * 1024)).object();
        require(data.value("version").toInt() == 1, "Unsupported reference manifest");
        const auto output = QFileInfo(parser.value("output"));
        require(QDir().mkpath(output.absolutePath()), "Cannot create verification output directory");
        AiOptions options;
        const auto policy = parser.value("provider");
        require(policy == "cpu" || policy == "prefer" || policy == "dml", "Unknown provider policy");
        options.policy = policy == "cpu" ? AiProviderPolicy::CpuOnly : policy == "dml" ? AiProviderPolicy::DirectMLOnly : AiProviderPolicy::PreferDirectML;
        bool validThreads = false, validAdapter = false;
        options.threads = parser.value("threads").toInt(&validThreads);
        options.adapterIndex = parser.value("adapter").toInt(&validAdapter);
        require(validThreads && options.threads > 0 && options.threads <= 64 && validAdapter && options.adapterIndex >= -1,
                "Invalid thread count or adapter index");
        options.disableMetacommands = parser.isSet("disable-metacommands");
        options.allowVendorMetacommandsOnIntel = parser.isSet("vendor-metacommands");
        bool validRepeat = false;
        const int repeat = parser.value("repeat").toInt(&validRepeat);
        require(validRepeat && repeat > 0 && repeat <= 20 && (!parser.isSet("no-profile") || policy == "cpu"),
                "Invalid repeat count, or profiling disabled for a GPU verification");
        report["profiling"] = !parser.isSet("no-profile");
        report["repeat"] = repeat;
        report["image_filter"] = parser.value("image");
        report["reverse_images"] = parser.isSet("reverse-images");
        report["threads"] = options.threads;
        report["disable_metacommands"] = options.disableMetacommands;
        report["python_reference_onnxruntime"] = data.value("onnxruntime");
        const auto definitions = data.value("models").toArray();
        require(!definitions.isEmpty(), "Reference manifest contains no models");
        for (const auto &definition : definitions) {
            const auto model = definition.toObject();
            const auto identifier = model.value("id").toString();
            if (parser.isSet("model") && identifier != parser.value("model")) continue;
            QJsonObject modelReport{{"id", identifier}, {"encoder_sha256", model.value("encoder_sha256")}};
            if (model.contains("decoder_sha256")) modelReport["decoder_sha256"] = model.value("decoder_sha256");
            QJsonArray imageReports;
            try {
                const auto imageKind = kind(model.value("kind").toString());
                const auto encoderPath = directory.filePath(model.value("encoder").toString());
                require(digest(encoderPath) == model.value("encoder_sha256").toString().toUtf8(), "Encoder SHA256 differs from Python reference");
                options.profilePrefix = QDir(output.absolutePath()).filePath("profile-" + identifier + "-" + policy + "-" + parser.value("adapter"));
                if (parser.isSet("no-profile")) options.profilePrefix.clear();
                auto encoder = imageKind == AiImageKind::BiRefNet ? AiSession::open(encoderPath, options) : std::shared_ptr<AiSession>();
                std::shared_ptr<AiSamModel> sam;
                if (!encoder) {
                    const auto decoderPath = directory.filePath(model.value("decoder").toString());
                    require(digest(decoderPath) == model.value("decoder_sha256").toString().toUtf8(), "Decoder SHA256 differs from Python reference");
                    sam = AiSamModel::open(encoderPath, decoderPath, imageKind, options);
                }
                auto imageRecords = model.value("images").toArray();
                if (parser.isSet("reverse-images")) {
                    QJsonArray reversed;
                    for (qsizetype index = imageRecords.size(); index > 0; --index)
                        reversed.append(imageRecords.at(index - 1));
                    imageRecords = reversed;
                }
                for (const auto &item : imageRecords) {
                    const auto imageRecord = item.toObject();
                    const auto filename = imageRecord.value("file").toString();
                    if (parser.isSet("image") && filename != parser.value("image")) continue;
                    require(QFileInfo(filename).fileName() == filename, "Unsafe reference image filename");
                    const QImage image(directory.filePath(filename));
                    require(!image.isNull(), "Cannot load reference image");
                    QElapsedTimer timer;
                    timer.start();
                    auto prepared = prepareAiImage(image, imageKind);
                    const double preprocessingMs = timer.nsecsElapsed() / 1e6;
                    check(checks, identifier + "/" + filename + "/preprocess",
                          compare(prepared.tensor, reference(directory, "image", imageRecord.value("input").toObject()), false, 1e-5, 0, 0));
                    QJsonObject imageReport{{"file", filename}, {"preprocess_ms", preprocessingMs}};
                    if (encoder) {
                        auto result = encoder->run({prepared.tensor});
                        compareOutputs(checks, directory, identifier + "/" + filename + "/encoder", result.outputs, imageRecord.value("encoder_outputs").toObject(), false);
                        imageReport["encoder"] = timing(result);
                        QJsonArray timings{result.milliseconds};
                        for (int iteration = 1; iteration < repeat; ++iteration) {
                            auto repeated = encoder->run({prepared.tensor});
                            compareOutputs(checks, directory, identifier + "/" + filename + "/repeat-" + QString::number(iteration + 1),
                                           repeated.outputs, imageRecord.value("encoder_outputs").toObject(), false);
                            timings.append(repeated.milliseconds);
                        }
                        imageReport["run_times_ms"] = timings;
                    } else {
                        timer.restart();
                        const auto encoded = sam->encodeOnce(image);
                        imageReport["encode_job_ms"] = timer.nsecsElapsed() / 1e6;
                        check(checks, identifier + "/" + filename + "/cache", {{"passed", encoded == sam->encodeOnce(image)}});
                        compareOutputs(checks, directory, identifier + "/" + filename + "/encoder", encoded->features, imageRecord.value("encoder_outputs").toObject(), false);
                        imageReport["encoder"] = timing(encoded->encoderResult);
                        QJsonArray clickReports;
                        AiTensor previous;
                        for (const auto &value : imageRecord.value("cases").toArray()) {
                            const auto click = value.toObject();
                            QVector<QPointF> points;
                            QVector<int> labels;
                            for (const auto &point : click.value("points").toArray()) {
                                const auto coordinates = point.toArray();
                                points.push_back({coordinates[0].toDouble(), coordinates[1].toDouble()});
                            }
                            for (const auto &label : click.value("labels").toArray()) labels.push_back(label.toInt());
                            auto result = sam->decode(encoded, points, labels, click.value("refine").toBool() ? previous : AiTensor());
                            compareOutputs(checks, directory, identifier + "/" + filename + "/" + click.value("name").toString(), result.outputs, click.value("outputs").toObject(), true);
                            const auto scores = result.outputs[1].floatValues();
                            const auto expectedScores = reference(directory, "iou_predictions", click.value("outputs").toObject().value("iou_predictions").toObject()).floatValues();
                            const int first = imageKind == AiImageKind::MobileSAM ? 1 : 0;
                            require(scores.size() > first && scores.size() == expectedScores.size(), "Invalid SAM mask scores");
                            const int selected = int(std::max_element(scores.begin() + first, scores.end()) - scores.begin());
                            const int expectedSelected = int(std::max_element(expectedScores.begin() + first, expectedScores.end()) - expectedScores.begin());
                            check(checks, identifier + "/" + filename + "/" + click.value("name").toString() + "/chosen_mask",
                                  {{"passed", selected == expectedSelected}, {"candidate", selected}, {"reference_candidate", expectedSelected}});
                            auto clickReport = timing(result);
                            clickReport["case"] = click.value("name");
                            clickReports.append(clickReport);
                            const auto &low = result.outputs[2];
                            const int candidate = click.value("feedback_candidate").toInt();
                            previous = {"feedback", {1, 1, 256, 256}, AiTensorType::Float32, low.bytes.mid(qsizetype(candidate) * 256 * 256 * 4, 256 * 256 * 4)};
                        }
                        imageReport["clicks"] = clickReports;
                    }
                    imageReports.append(imageReport);
                }
                require(!imageReports.isEmpty(), "No reference image matches the requested filename");
                if (encoder) {
                    modelReport["load_ms"] = encoder->initializationMilliseconds();
                    modelReport["encoder_profile"] = profile(encoder->finishProfiling());
                } else {
                    modelReport["encoder_runs"] = double(sam->encoderRuns());
                    modelReport["load_ms"] = sam->encoderSession()->initializationMilliseconds() + sam->decoderSession()->initializationMilliseconds();
                    modelReport["encoder_profile"] = profile(sam->encoderSession()->finishProfiling());
                    modelReport["decoder_profile"] = profile(sam->decoderSession()->finishProfiling());
                }
                if (policy == "dml") {
                    for (const auto &name : {"encoder_profile", "decoder_profile"}) {
                        if (!modelReport.contains(name)) continue;
                        const auto executed = modelReport.value(name).toObject().value("executed_nodes").toObject();
                        check(checks, identifier + "/" + name + "/gpu_execution", {{"passed", executed.value("DmlExecutionProvider").toInt() > 0}, {"nodes", executed}});
                    }
                }
            } catch (const std::exception &error) {
                modelReport["error"] = QString::fromUtf8(error.what());
                check(checks, identifier + "/runtime", {{"passed", false}, {"reason", QString::fromUtf8(error.what())}});
            }
            modelReport["images"] = imageReports;
            models.append(modelReport);
        }
        require(!models.isEmpty(), "No reference model matches the requested ID");
        bool passed = true;
        for (const auto &value : checks) passed &= value.toObject().value("passed").toBool();
        report["status"] = passed ? "passed" : "failed";
    } catch (const std::exception &error) {
        report["error"] = QString::fromUtf8(error.what());
    }
    report["checks"] = checks;
    report["models"] = models;
    const auto bytes = QJsonDocument(report).toJson();
    if (parser.isSet("output")) {
        QFile file(parser.value("output"));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            std::fprintf(stderr, "Cannot save AI verification report\n");
            return 1;
        }
    }
    std::puts(bytes.constData());
    return report.value("status").toString() == "passed" ? 0 : 1;
}
