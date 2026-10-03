#include "ai_session.h"
#include "document.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QScopeGuard>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <dml_provider_factory.h>
#include <dxgi1_6.h>
#include <onnxruntime_cxx_api.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace compositor {
namespace {
qint64 typeSize(AiTensorType type) {
    return type == AiTensorType::Int64 ? 8 : 4;
}
ONNXTensorElementDataType ortType(AiTensorType type) {
    switch (type) {
    case AiTensorType::Float32: return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    case AiTensorType::Int32: return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
    case AiTensorType::Int64: return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    }
    throw Error("Unsupported AI tensor type");
}
AiTensorType aiType(ONNXTensorElementDataType type) {
    switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return AiTensorType::Float32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: return AiTensorType::Int32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return AiTensorType::Int64;
    default: throw Error("Model requires an unsupported tensor type");
    }
}
Ort::Env &environment() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "Compositor AI");
    return env;
}
AiTensor makeTensor(QString name, QVector<qint64> shape, AiTensorType type, const void *values, qsizetype count) {
    AiTensor tensor{std::move(name), std::move(shape), type, QByteArray(static_cast<const char *>(values), count * typeSize(type))};
    tensor.validate();
    return tensor;
}
}
qint64 AiTensor::elements() const {
    qint64 total = 1;
    for (auto dimension : shape) {
        require(dimension > 0 && total <= MaxAiTensorElements / dimension, "AI tensor exceeds its memory budget");
        total *= dimension;
    }
    return total;
}
void AiTensor::validate() const {
    require(!name.isEmpty() && bytes.size() == elements() * typeSize(type), "Invalid AI tensor binding");
}
QVector<float> AiTensor::floatValues() const {
    validate();
    require(type == AiTensorType::Float32, "AI tensor is not float32");
    QVector<float> result(elements());
    std::memcpy(result.data(), bytes.constData(), size_t(bytes.size()));
    return result;
}
AiTensor AiTensor::floats(QString name, QVector<qint64> shape, const QVector<float> &values) {
    return makeTensor(std::move(name), std::move(shape), AiTensorType::Float32, values.constData(), values.size());
}
AiTensor AiTensor::integers(QString name, QVector<qint64> shape, const QVector<qint32> &values) {
    return makeTensor(std::move(name), std::move(shape), AiTensorType::Int32, values.constData(), values.size());
}
AiTensor AiTensor::sizes(QString name, QVector<qint64> shape, const QVector<qint64> &values) {
    return makeTensor(std::move(name), std::move(shape), AiTensorType::Int64, values.constData(), values.size());
}
void AiCancellation::cancel() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard lock(mutex_);
        cancelled_.store(true);
        for (const auto &[id, callback] : interrupts_) {
            Q_UNUSED(id);
            callbacks.push_back(callback);
        }
    }
    for (auto &callback : callbacks)
        callback();
}
bool AiCancellation::cancelled() const {
    return cancelled_.load();
}
void AiCancellation::check() const {
    if (cancelled())
        throw AiCancelled();
}
quint64 AiCancellation::attach(std::function<void()> interrupt) {
    quint64 id;
    bool stop;
    {
        std::lock_guard lock(mutex_);
        id = ++next_;
        interrupts_[id] = interrupt;
        stop = cancelled();
    }
    if (stop)
        interrupt();
    return id;
}
void AiCancellation::detach(quint64 id) {
    std::lock_guard lock(mutex_);
    interrupts_.erase(id);
}
int aiDefaultThreads() {
    // Bound background CPU inference while avoiding the four-thread bottleneck on larger CPUs.
    return std::clamp(QThread::idealThreadCount() / 2, 1, 12);
}
QString aiRuntimeVersion() {
    return QString::fromUtf8(OrtGetApiBase()->GetVersionString());
}
QList<AiAdapter> aiAdapters() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))))
        return {};
    QList<AiAdapter> result;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
            break;
        if (!adapter)
            continue;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)))
            continue;
        ComPtr<ID3D12Device> device;
        const bool software = description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE;
        const bool supported = !software && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())));
        result.push_back({int(index), QString::fromWCharArray(description.Description), description.VendorId, quint64(description.DedicatedVideoMemory), supported, software});
    }
    return result;
}
struct AiSession::Impl {
    QString path;
    AiOptions options;
    QString provider = "CPU", adapter, fallback;
    double initializationMs = 0;
    bool profiling = false, metacommandsDisabled = false;
    mutable std::mutex mutex;
    Ort::Session session{nullptr};
    void create(bool directML) {
        Ort::SessionOptions configuration;
        configuration.SetIntraOpNumThreads(options.threads);
        configuration.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        configuration.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        if (directML) {
            const auto devices = aiAdapters();
            int index = options.adapterIndex;
            quint64 memory = 0;
            if (index < 0) {
                for (const auto &device : devices)
                    if (device.directX12 && (index < 0 || device.dedicatedMemory > memory)) {
                        index = device.index;
                        memory = device.dedicatedMemory;
                    }
            }
            auto found = std::find_if(devices.begin(), devices.end(), [index](const auto &device) { return device.index == index && device.directX12; });
            require(found != devices.end(), "No compatible DirectX 12 GPU is available for the selected adapter");
            const OrtDmlApi *dml = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void **>(&dml)));
            configuration.DisableMemPattern();
            // Intel's vendor kernels exceed MobileSAM's FP32 fidelity tolerance on our fixtures.
            metacommandsDisabled = options.disableMetacommands ||
                                   (found->vendor == 0x8086 && !options.allowVendorMetacommandsOnIntel);
            if (metacommandsDisabled) {
                const auto device = QByteArray::number(index);
                const char *keys[]{"device_id", "disable_metacommands"};
                const char *values[]{device.constData(), "true"};
                Ort::ThrowOnError(Ort::GetApi().SessionOptionsAppendExecutionProvider(configuration, "DML", keys, values, 2));
            } else {
                Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(configuration, index));
            }
            adapter = found->name;
        }
        if (!options.profilePrefix.isEmpty()) {
            configuration.EnableProfiling(options.profilePrefix.toStdWString().c_str());
            profiling = true;
        }
        session = Ort::Session(environment(), path.toStdWString().c_str(), configuration);
        provider = directML ? "DirectML+CPU" : "CPU";
        if (!directML) {
            adapter.clear();
            metacommandsDisabled = false;
        }
    }
    AiRunResult execute(const QList<AiTensor> &inputs, const std::shared_ptr<Ort::RunOptions> &runOptions) {
        Ort::AllocatorWithDefaultOptions allocator;
        require(size_t(inputs.size()) == session.GetInputCount(), "AI input count does not match the model");
        std::vector<Ort::Value> tensors;
        std::vector<std::string> names;
        for (size_t i = 0; i < session.GetInputCount(); ++i) {
            const auto expectedName = session.GetInputNameAllocated(i, allocator);
            auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto &input) { return input.name == QString::fromUtf8(expectedName.get()); });
            require(found != inputs.end(), "Missing AI input: " + QString::fromUtf8(expectedName.get()));
            found->validate();
            const auto expectedType = session.GetInputTypeInfo(i);
            auto expected = expectedType.GetTensorTypeAndShapeInfo();
            const auto dimensions = expected.GetShape();
            require(expected.GetElementType() == ortType(found->type) && dimensions.size() == size_t(found->shape.size()), "AI input type or rank does not match the model");
            for (size_t axis = 0; axis < dimensions.size(); ++axis)
                require(dimensions[axis] < 0 || dimensions[axis] == found->shape[qsizetype(axis)], "AI input dimensions do not match the model");
            std::vector<int64_t> shape(found->shape.begin(), found->shape.end());
            tensors.push_back(Ort::Value::CreateTensor(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault), const_cast<char *>(found->bytes.constData()), size_t(found->bytes.size()), shape.data(), shape.size(), ortType(found->type)));
            names.emplace_back(expectedName.get());
        }
        std::vector<std::string> outputNames;
        for (size_t i = 0; i < session.GetOutputCount(); ++i)
            outputNames.emplace_back(session.GetOutputNameAllocated(i, allocator).get());
        std::vector<const char *> inputPointers, outputPointers;
        for (auto &name : names) inputPointers.push_back(name.c_str());
        for (auto &name : outputNames) outputPointers.push_back(name.c_str());
        QElapsedTimer timer;
        timer.start();
        auto outputs = session.Run(*runOptions, inputPointers.data(), tensors.data(), tensors.size(), outputPointers.data(), outputPointers.size());
        AiRunResult result{{}, provider, adapter, fallback, timer.nsecsElapsed() / 1e6, metacommandsDisabled};
        for (size_t i = 0; i < outputs.size(); ++i) {
            require(outputs[i].IsTensor(), "AI model returned a non-tensor output");
            const auto information = outputs[i].GetTensorTypeAndShapeInfo();
            const auto dimensions = information.GetShape();
            AiTensor tensor{QString::fromStdString(outputNames[i]), QVector<qint64>(dimensions.begin(), dimensions.end()), aiType(information.GetElementType()), {}};
            const auto bytes = tensor.elements() * typeSize(tensor.type);
            tensor.bytes = QByteArray(static_cast<const char *>(outputs[i].GetTensorRawData()), bytes);
            tensor.validate();
            result.outputs.push_back(std::move(tensor));
        }
        return result;
    }
};
AiSession::AiSession() : impl_(std::make_unique<Impl>()) {}
AiSession::~AiSession() = default;
std::shared_ptr<AiSession> AiSession::open(const QString &path, const AiOptions &options) {
    require(QFileInfo(path).isFile() && options.threads > 0, "AI model is missing or its session options are invalid");
    auto result = std::shared_ptr<AiSession>(new AiSession);
    result->impl_->path = path;
    result->impl_->options = options;
    QElapsedTimer timer;
    timer.start();
    try {
        result->impl_->create(options.policy != AiProviderPolicy::CpuOnly);
    } catch (const std::exception &error) {
        if (options.policy != AiProviderPolicy::PreferDirectML)
            throw;
        result->impl_->fallback = QString::fromUtf8(error.what());
        result->impl_->create(false);
    }
    result->impl_->initializationMs = timer.nsecsElapsed() / 1e6;
    return result;
}
QFuture<std::shared_ptr<AiSession>> AiSession::openAsync(QString path, AiOptions options) {
    return QtConcurrent::run([path = std::move(path), options] { return open(path, options); });
}
AiRunResult AiSession::run(const QList<AiTensor> &inputs, std::shared_ptr<AiCancellation> cancellation) {
    if (!cancellation)
        cancellation = std::make_shared<AiCancellation>();
    cancellation->check();
    std::lock_guard lock(impl_->mutex);
    cancellation->check();
    auto runOptions = std::make_shared<Ort::RunOptions>();
    const auto id = cancellation->attach([runOptions] {
        if (auto status = Ort::GetApi().RunOptionsSetTerminate(*runOptions))
            Ort::GetApi().ReleaseStatus(status);
    });
    auto release = qScopeGuard([&] { cancellation->detach(id); });
    try {
        auto result = impl_->execute(inputs, runOptions);
        cancellation->check();
        return result;
    } catch (const Ort::Exception &error) {
        cancellation->check();
        if (impl_->options.policy != AiProviderPolicy::PreferDirectML || impl_->provider == "CPU")
            throw;
        impl_->fallback = QString::fromUtf8(error.what());
        impl_->session = Ort::Session(nullptr);
        impl_->create(false);
        cancellation->check();
        auto result = impl_->execute(inputs, runOptions);
        cancellation->check();
        return result;
    }
}
QFuture<AiRunResult> AiSession::runAsync(QList<AiTensor> inputs, std::shared_ptr<AiCancellation> cancellation) {
    return QtConcurrent::run([self = shared_from_this(), inputs = std::move(inputs), cancellation] { return self->run(inputs, cancellation); });
}
QString AiSession::backend() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->provider;
}
QString AiSession::fallbackReason() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->fallback;
}
double AiSession::initializationMilliseconds() const {
    return impl_->initializationMs;
}
QString AiSession::finishProfiling() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->profiling)
        return {};
    Ort::AllocatorWithDefaultOptions allocator;
    impl_->profiling = false;
    return QString::fromUtf8(impl_->session.EndProfilingAllocated(allocator).get());
}
}
