// SPDX-License-Identifier: MIT
#pragma once
#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <functional>

namespace compositor {
// Computes live previews off the UI thread. One job runs at a time; a request made while one is
// running replaces whichever was waiting, so dragging a slider never queues up stale work. The
// running job cannot be interrupted, but `cancel` discards its result and anything waiting.
class PreviewRunner : public QObject {
    Q_OBJECT
  public:
    using Job = std::function<QImage()>;
    explicit PreviewRunner(QObject *parent = nullptr);
    ~PreviewRunner() override;
    void request(Job job);
    void cancel();
    bool busy() const {
        return running_;
    }
    // Whether a result newer than the last one shown is still to come.
    bool pending() const {
        return running_ || waiting_;
    }

  signals:
    // A finished job's image. Results of jobs that a later request has overtaken are still
    // delivered, so the preview keeps moving while the user drags.
    void ready(const QImage &image);
    void failed(const QString &message);
    void busyChanged(bool busy);

  private:
    struct Result {
        QImage image;
        QString error;
    };
    QFutureWatcher<Result> watcher_;
    Job waitingJob_;
    bool running_ = false, waiting_ = false;
    quint64 epoch_ = 0, runningEpoch_ = 0;
    void start(Job job);
    void finished();
};
} // namespace compositor
