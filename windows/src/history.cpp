// SPDX-License-Identifier: MIT
#include "history.h"
#include <QSignalBlocker>
#include <algorithm>
#include <vector>

namespace compositor {
qint64 HistoryStack::usage(int start) const {
    QHash<qint64, qint64> images;
    qint64 bytes = 0;
    for (int i = start; i < count(); ++i)
        static_cast<const HistoryCommand *>(command(i))->memory(images, bytes);
    for (auto size : images)
        bytes += size;
    return bytes;
}
qint64 HistoryStack::memoryUsage() const {
    return usage(0);
}
void HistoryStack::setMemoryBudget(qint64 bytes) {
    budget_ = std::max(qint64(0), bytes);
    trim();
}
void HistoryStack::push(HistoryCommand *command) {
    QUndoStack::push(command);
    trim();
}
void HistoryStack::trim() {
    int remove = std::max(0, count() - 40);
    while (remove < count() && usage(remove) > budget_)
        ++remove;
    if (!remove)
        return;
    // Preserve the redo chain after an undo. If trimming crosses the current state, those
    // commands cannot be rebased safely, so discard the entire history without changing pixels.
    if (remove > index())
        remove = count();
    const int oldIndex = index(), oldClean = cleanIndex();
    std::vector<std::unique_ptr<HistoryCommand>> retained;
    for (int i = remove; i < count(); ++i)
        retained.push_back(static_cast<const HistoryCommand *>(command(i))->copy());
    rebuilding_ = true;
    {
        QSignalBlocker blocker(this);
        clear();
        for (auto &command : retained)
            QUndoStack::push(command.release());
        if (oldClean >= remove) {
            setIndex(oldClean - remove);
            setClean();
        } else
            resetClean();
        setIndex(std::max(0, oldIndex - remove));
    }
    rebuilding_ = false;
    emit indexChanged(index());
    emit canUndoChanged(canUndo());
    emit canRedoChanged(canRedo());
    emit undoTextChanged(undoText());
    emit redoTextChanged(redoText());
    emit cleanChanged(isClean());
}
} // namespace compositor
