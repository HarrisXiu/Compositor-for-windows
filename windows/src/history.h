// SPDX-License-Identifier: MIT
#pragma once
#include <QHash>
#include <QImage>
#include <QUndoStack>
#include <memory>
#include <optional>

namespace compositor {
class HistoryCommand : public QUndoCommand {
  public:
    using QUndoCommand::QUndoCommand;
    virtual std::unique_ptr<HistoryCommand> copy() const = 0;
    virtual void memory(QHash<qint64, qint64> &images, qint64 &metadata) const = 0;
    // What the command holds, worked out once: a command never changes after it is pushed, and
    // measuring it serializes both document snapshots.
    struct Footprint {
        QHash<qint64, qint64> images;
        qint64 metadata = 0;
        // Everything counted as if nothing were shared with other commands: an upper bound.
        qint64 total = 0;
    };
    const Footprint &footprint() const {
        if (!footprint_) {
            Footprint measured;
            memory(measured.images, measured.metadata);
            measured.total = measured.metadata;
            for (auto size : measured.images)
                measured.total += size;
            footprint_ = std::move(measured);
        }
        return *footprint_;
    }

  private:
    mutable std::optional<Footprint> footprint_;
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
    // Drops the oldest commands past the budget. After a push the newest command is kept even
    // when it alone exceeds the budget, so the edit just made can still be undone.
    void trim(bool keepNewest = false);
    qint64 usage(int start) const;
    qint64 budget_ = 512LL * 1024 * 1024;
    bool rebuilding_ = false;
};
inline void countImage(const QImage &image, QHash<qint64, qint64> &images) {
    if (!image.isNull())
        images.insert(image.cacheKey(), image.sizeInBytes());
}
} // namespace compositor
