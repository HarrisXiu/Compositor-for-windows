// SPDX-License-Identifier: MIT
#pragma once
#include <QHash>
#include <QImage>
#include <QUndoStack>
#include <memory>

namespace compositor {
class HistoryCommand : public QUndoCommand {
  public:
    using QUndoCommand::QUndoCommand;
    virtual std::unique_ptr<HistoryCommand> copy() const = 0;
    virtual void memory(QHash<qint64, qint64> &images, qint64 &metadata) const = 0;
};
class HistoryStack final : public QUndoStack {
  public:
    explicit HistoryStack(QObject *parent = nullptr) : QUndoStack(parent) {}
    void push(HistoryCommand *command);
    void setMemoryBudget(qint64 bytes);
    qint64 memoryUsage() const;
    qint64 memoryBudget() const {
        return budget_;
    }
    bool rebuilding() const {
        return rebuilding_;
    }

  private:
    void trim();
    qint64 usage(int start) const;
    qint64 budget_ = 512LL * 1024 * 1024;
    bool rebuilding_ = false;
};
inline void countImage(const QImage &image, QHash<qint64, qint64> &images) {
    if (!image.isNull())
        images.insert(image.cacheKey(), image.sizeInBytes());
}
} // namespace compositor
