// SPDX-License-Identifier: MIT
#include "preview_runner.h"
#include <QtConcurrent/QtConcurrentRun>
#include <exception>

namespace compositor {
PreviewRunner::PreviewRunner(QObject *parent) : QObject(parent) {
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, &PreviewRunner::finished);
}
PreviewRunner::~PreviewRunner() {
    // A job still running finishes on its own; nothing it holds belongs to this object.
    disconnect(&watcher_, nullptr, this, nullptr);
}
void PreviewRunner::request(Job job) {
    ++epoch_;
    if (running_) {
        waitingJob_ = std::move(job);
        waiting_ = true;
        return;
    }
    start(std::move(job));
}
void PreviewRunner::cancel() {
    ++epoch_;
    waitingJob_ = {};
    waiting_ = false;
}
void PreviewRunner::start(Job job) {
    runningEpoch_ = epoch_;
    const bool changed = !running_;
    running_ = true;
    watcher_.setFuture(QtConcurrent::run([job = std::move(job)] {
        Result result;
        try {
            result.image = job();
        } catch (const std::exception &e) {
            result.error = QString::fromUtf8(e.what());
        }
        return result;
    }));
    if (changed)
        emit busyChanged(true);
}
void PreviewRunner::finished() {
    const auto result = watcher_.result();
    const bool wanted = runningEpoch_ == epoch_ || waiting_;
    running_ = false;
    if (waiting_) {
        auto next = std::move(waitingJob_);
        waitingJob_ = {};
        waiting_ = false;
        // Show this result while the newer job computes.
        if (result.error.isEmpty())
            emit ready(result.image);
        else
            emit failed(result.error);
        start(std::move(next));
        return;
    }
    emit busyChanged(false);
    if (!wanted)
        return;
    if (result.error.isEmpty())
        emit ready(result.image);
    else
        emit failed(result.error);
}
} // namespace compositor
