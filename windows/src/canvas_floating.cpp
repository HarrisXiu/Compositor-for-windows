// SPDX-License-Identifier: MIT
#include "canvas.h"
#include "canvas_tools.h"
#include "selection_float.h"
#include <QJsonArray>
#include <QMouseEvent>
#include <cmath>

namespace compositor {
namespace {
bool hasCoverage(const QImage &mask) {
    for (int y = 0; y < mask.height(); ++y) {
        const auto row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            if (row[x])
                return true;
    }
    return false;
}
} // namespace

bool Canvas::canTransformSelection() const {
    if (floating_ || session_->selection.isNull() || paintMask())
        return false;
    const auto *layer = document_->active();
    return layer && !layer->group() && !layer->image.isNull() && hasCoverage(session_->selection);
}
void Canvas::startEdit() {
    if (!floating_)
        emit editStarted();
}
void Canvas::finishEdit(const QString &label) {
    if (floating_) {
        emit edited();
        refresh();
    } else
        emit editFinished(label);
}
// Puts back what a gesture inside a floating selection had changed, without ending the selection.
void Canvas::restoreGesture() {
    for (auto it = transformOriginals_.cbegin(); it != transformOriginals_.cend(); ++it)
        if (auto *layer = document_->find(it.key()))
            *layer = it.value();
    refresh();
}
void Canvas::beginSelectionTransform() {
    if (floating_)
        return;
    require(canTransformSelection(), "Select a pixel layer and make a selection to transform");
    setTool(Tool::Move);
    cancelInteraction();
    const auto sourceId = document_->activeId();
    emit editStarted();
    const auto id = liftSelection(*document_, sourceId, session_->selection, false);
    if (id.isEmpty()) {
        emit editCanceled();
        throw Error("The selection covers no pixels of this layer");
    }
    floating_ = FloatingSession{id, sourceId};
    document_->metadata["activeLayerID"] = id;
    session_->selectedLayerIDs = {id};
    session_->target = EditTarget::Pixels;
    session_->view.transformControls = true;
    refresh();
    emit sessionChanged();
    emit selectionChanged();
    update();
}
void Canvas::commitFloatingSelection() {
    if (!floating_)
        return;
    cancelInteraction();
    const auto pending = *floating_;
    floating_.reset();
    try {
        const auto *floating = document_->find(pending.floatingId);
        require(floating, "Floating selection is no longer available");
        auto selection = floatingSelection(*document_, *floating);
        mergeFloatingLayer(*document_, pending.floatingId, pending.sourceId);
        session_->selection = hasCoverage(selection) ? selection : QImage();
        session_->selectedLayerIDs = {pending.sourceId};
    } catch (const std::exception &ex) {
        endLayerEdit();
        emit editCanceled();
        emit error(QString::fromUtf8(ex.what()));
        emit sessionChanged();
        emit selectionChanged();
        return;
    }
    endLayerEdit();
    emit editFinished("Transform Selection");
    refresh();
    emit sessionChanged();
    emit selectionChanged();
}
void Canvas::cancelFloatingSelection() {
    if (!floating_)
        return;
    cancelInteraction();
    floating_.reset();
    endLayerEdit();
    emit editCanceled();
    refresh();
    emit sessionChanged();
    emit selectionChanged();
}

bool Canvas::beginSelectionDrag(QMouseEvent *e) {
    const auto tool = session_->tool;
    if (tool != Tool::RectangleSelect && tool != Tool::EllipseSelect && tool != Tool::Lasso &&
        tool != Tool::Wand)
        return false;
    if (floating_ || session_->selection.isNull() || !polygonPoints_.isEmpty())
        return false;
    const QPoint pixel(int(std::floor(start_.x())), int(std::floor(start_.y())));
    if (!session_->selection.valid(pixel) || session_->selection.constScanLine(pixel.y())[pixel.x()] < 128)
        return false;
    const auto modifiers = e->modifiers();
    const bool pixels = modifiers & Qt::ControlModifier;
    // Without Ctrl, only a plain drag in Replace mode moves the outline; Shift and Alt add to or
    // subtract from the selection as they always do.
    if (!pixels && ((modifiers & (Qt::ShiftModifier | Qt::AltModifier)) || session_->selectionMode != 0))
        return false;
    priorSelection_ = session_->selection;
    dragOffset_ = {};
    if (!pixels) {
        selectionDrag_ = SelectionDrag::Outline;
        dragging_ = true;
        return true;
    }
    const auto *layer = document_->active();
    require(layer && !layer->group() && !layer->image.isNull() && !paintMask(),
            "Select a pixel layer to move its selected pixels");
    dragSourceId_ = layer->id();
    dragDuplicate_ = modifiers & Qt::AltModifier;
    emit editStarted();
    dragFloatingId_ = liftSelection(*document_, dragSourceId_, session_->selection, dragDuplicate_);
    if (dragFloatingId_.isEmpty()) {
        emit editCanceled();
        throw Error("The selection covers no pixels of this layer");
    }
    dragOrigin_ = document_->find(dragFloatingId_)->transform();
    selectionDrag_ = SelectionDrag::Pixels;
    dragging_ = true;
    beginLayerEdit(dragFloatingId_);
    refresh();
    return true;
}
void Canvas::moveSelectedPixels(QPoint offset) {
    auto *floating = document_->find(dragFloatingId_);
    if (!floating)
        return;
    auto transform = dragOrigin_;
    const auto origin = dragOrigin_.value("origin").toArray();
    transform["origin"] = QJsonArray{origin.at(0).toDouble() + offset.x(),
                                     origin.at(1).toDouble() + offset.y()};
    floating->metadata["transform"] = transform;
    emit edited();
    refresh();
}
void Canvas::updateSelectionDrag(QMouseEvent *e) {
    auto delta = toDocument(e->position()) - start_;
    if (e->modifiers() & Qt::ShiftModifier) {
        if (std::abs(delta.x()) >= std::abs(delta.y()))
            delta.setY(0);
        else
            delta.setX(0);
    }
    const QPoint offset(int(std::lround(delta.x())), int(std::lround(delta.y())));
    if (offset == dragOffset_)
        return;
    dragOffset_ = offset;
    if (selectionDrag_ == SelectionDrag::Outline) {
        session_->selection = shiftSelection(priorSelection_, offset);
        update();
    } else
        moveSelectedPixels(offset);
}
void Canvas::finishSelectionDrag(QMouseEvent *event) {
    const auto kind = std::exchange(selectionDrag_, SelectionDrag::None);
    const auto offset = std::exchange(dragOffset_, QPoint());
    const auto prior = std::exchange(priorSelection_, QImage());
    if (kind == SelectionDrag::Outline) {
        const auto after = session_->selection;
        session_->selection = prior;
        if (offset != QPoint())
            replaceSelection(after, "Move Selection");
        else if (session_->tool == Tool::Wand)
            controller().press(event); // A click inside the selection selects afresh from that pixel.
        else
            replaceSelection({}, "Deselect"); // A click without a drag deselects.
        update();
        return;
    }
    if (kind != SelectionDrag::Pixels)
        return;
    // Letting go where it was grabbed changes nothing.
    if (offset == QPoint()) {
        endLayerEdit();
        emit editCanceled();
        return;
    }
    mergeFloatingLayer(*document_, dragFloatingId_, dragSourceId_);
    session_->selection = shiftSelection(prior, offset);
    session_->selectedLayerIDs = {dragSourceId_};
    endLayerEdit();
    emit editFinished(dragDuplicate_ ? "Duplicate Selected Pixels" : "Move Selected Pixels");
    emit selectionChanged();
    refresh();
}
} // namespace compositor
