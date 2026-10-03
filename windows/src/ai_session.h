#pragma once
#include <QByteArray>
#include <QFuture>
#include <QException>
#include <QList>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace compositor {
constexpr qint64 MaxAiTensorElements = 200000000;
enum class AiTensorType { Float32, Int32, Int64 };
struct AiTensor {
    QString name;
    QVector<qint64> shape;
    AiTensorType type = AiTensorType::Float32;
    QByteArray bytes;
    qint64 elements() const;
    void validate() const;
    QVector<float> floatValues() const;
    static AiTensor floats(QString name, QVector<qint64> shape, const QVector<float> &values);
    static AiTensor integers(QString name, QVector<qint64> shape, const QVector<qint32> &values);
    static AiTensor sizes(QString name, QVector<qint64> shape, const QVector<qint64> &values);
};
class AiCancelled : public QException {
  public:
    const char *what() const noexcept override { return "AI inference cancelled"; }
    void raise() const override { throw *this; }
    AiCancelled *clone() const override { return new AiCancelled(*this); }
};
class AiCancellation {
  public:
    void cancel();
    bool cancelled() const;
    void check() const;
  private:
    friend class AiSession;
    quint64 attach(std::function<void()> interrupt);
    void detach(quint64 id);
    std::atomic_bool cancelled_{false};
    std::mutex mutex_;
    quint64 next_ = 0;
    std::map<quint64, std::function<void()>> interrupts_;
};
enum class AiProviderPolicy { PreferDirectML, CpuOnly, DirectMLOnly };
int aiDefaultThreads();
struct AiOptions {
    AiProviderPolicy policy = AiProviderPolicy::PreferDirectML;
    int adapterIndex = -1;
    int threads = aiDefaultThreads();
    QString profilePrefix;
    bool disableMetacommands = false;
    bool allowVendorMetacommands = false;
    bool disableMetacommandsOnAmd = false;
    bool disableDmlGraphFusion = false;
    bool disableDmlMemoryArena = false;
};
struct AiAdapter {
    int index = -1;
    QString name;
    quint32 vendor = 0;
    quint64 dedicatedMemory = 0;
    bool directX12 = false;
    bool software = false;
};
QList<AiAdapter> aiAdapters();
QString aiRuntimeVersion();
struct AiRunResult {
    QList<AiTensor> outputs;
    QString backend;
    QString adapter;
    QString fallbackReason;
    double milliseconds = 0;
    bool metacommandsDisabled = false;
};
class AiSession : public std::enable_shared_from_this<AiSession> {
  public:
    static std::shared_ptr<AiSession> open(const QString &path, const AiOptions &options = {});
    static QFuture<std::shared_ptr<AiSession>> openAsync(QString path, AiOptions options = {});
    ~AiSession();
    AiRunResult run(const QList<AiTensor> &inputs, std::shared_ptr<AiCancellation> cancellation = {});
    QFuture<AiRunResult> runAsync(QList<AiTensor> inputs, std::shared_ptr<AiCancellation> cancellation = {});
    QString backend() const;
    QString fallbackReason() const;
    double initializationMilliseconds() const;
    QString finishProfiling();
  private:
    AiSession();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
