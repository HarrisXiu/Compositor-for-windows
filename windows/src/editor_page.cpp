// SPDX-License-Identifier: MIT
#include "editor.h"
#include <QUndoCommand>
#include <QVBoxLayout>

namespace compositor {
class DocumentCommand final : public QUndoCommand {
  public:
    DocumentCommand(EditorPage *page, QString label, Document before, Document after,
                    QImage beforeSelection, QImage afterSelection, quint64 afterState)
        : QUndoCommand(label), page_(page), before_(std::move(before)), after_(std::move(after)),
          beforeSelection_(std::move(beforeSelection)), afterSelection_(std::move(afterSelection)),
          beforeState_(page->contentState), afterState_(afterState) {}
    QSet<QString> beforeLayers, afterLayers;
    void undo() override {
        page_->session.selectedLayerIDs = beforeLayers;
        apply(before_, beforeSelection_, beforeState_);
    }
    void redo() override {
        page_->session.selectedLayerIDs = afterLayers;
        apply(after_, afterSelection_, afterState_);
    }

  private:
    void apply(const Document &document, const QImage &selection, quint64 state) {
        if (page_->document.size() != document.size())
            page_->session.cropFrame = {};
        page_->document = document;
        page_->session.selection = selection;
        page_->contentState = state;
        page_->changed();
        emit page_->canvas->selectionChanged();
    }
    EditorPage *page_;
    Document before_, after_;
    QImage beforeSelection_, afterSelection_;
    quint64 beforeState_, afterState_;
};
class SelectionCommand final : public QUndoCommand {
  public:
    SelectionCommand(EditorPage *page, QString label, QImage before, QImage after)
        : QUndoCommand(label), page_(page), before_(std::move(before)), after_(std::move(after)) {}
    void undo() override {
        apply(before_);
    }
    void redo() override {
        apply(after_);
    }

  private:
    void apply(const QImage &selection) {
        page_->session.selection = selection;
        page_->canvas->update();
        emit page_->canvas->selectionChanged();
        emit page_->documentChanged();
    }
    EditorPage *page_;
    QImage before_, after_;
};
EditorPage::EditorPage(Document source, QWidget *parent)
    : QWidget(parent), document(std::move(source)), history(this) {
    canvas = new Canvas(&document, &session, this);
    if (!document.activeId().isEmpty())
        session.selectedLayerIDs.insert(document.activeId());
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(canvas);
    history.setUndoLimit(40);
    connect(canvas, &Canvas::editStarted, this, [this] {
        beforeInteraction_ = document;
        beforeSelection_ = session.selection;
        beforeInteractionLayers_ = session.selectedLayerIDs;
        interacting_ = true;
    });
    connect(canvas, &Canvas::editFinished, this, [this](const QString &label) {
        if (!interacting_)
            return;
        interacting_ = false;
        record(label, beforeInteraction_, document, beforeSelection_, session.selection,
               beforeInteractionLayers_, session.selectedLayerIDs);
        beforeInteraction_ = {};
        beforeSelection_ = {};
    });
    auto rollback = [this] {
        if (interacting_) {
            document = beforeInteraction_;
            session.selection = beforeSelection_;
            session.selectedLayerIDs = beforeInteractionLayers_;
            interacting_ = false;
            beforeInteraction_ = {};
            beforeSelection_ = {};
            changed();
        }
    };
    connect(canvas, &Canvas::editCanceled, this, rollback);
    connect(canvas, &Canvas::error, this, [this, rollback](const QString &message) {
        rollback();
        emit error(message);
    });
    connect(canvas, &Canvas::selectionEdited, this,
            [this](const QString &label, const QImage &before, const QImage &after) {
                history.push(new SelectionCommand(this, label, before, after));
            });
    connect(&history, &QUndoStack::cleanChanged, this, [this] { emit documentChanged(); });
}
EditorPage::~EditorPage() {
    // The window that owns this page is tearing down its tabs; clearing the undo stack below must
    // not tell it to refresh panels it is already destroying.
    disconnect(this, &EditorPage::documentChanged, nullptr, nullptr);
    // Finish while the document/history still exist, before QWidget hides its children.
    canvas->finishTextEditing(false);
}
void EditorPage::changed() {
    QSet<QString> valid;
    for (const auto &layer : document.layers)
        if (session.selectedLayerIDs.contains(layer.id()))
            valid.insert(layer.id());
    if (!document.activeId().isEmpty())
        valid.insert(document.activeId());
    session.selectedLayerIDs = valid;
    if (!document.active() || document.active()->mask.isNull())
        session.target = EditTarget::Pixels;
    ++revision;
    canvas->refresh();
    emit documentChanged();
}
void EditorPage::markSaved(quint64 state) {
    savedContentState_ = state;
    if (!isModified())
        history.setClean();
    emit documentChanged();
}
void EditorPage::record(const QString &label, const Document &before, const Document &after,
                        const QImage &beforeSelection, const QImage &afterSelection,
                        const QSet<QString> &beforeLayers, const QSet<QString> &afterLayers) {
    try {
        after.validateAssets();
        auto command = new DocumentCommand(this, label, before, after, beforeSelection,
                                           afterSelection, ++nextContentState_);
        command->beforeLayers = beforeLayers.isEmpty() && !before.activeId().isEmpty()
                                    ? QSet<QString>{before.activeId()}
                                    : beforeLayers;
        command->afterLayers = afterLayers.isEmpty() && !after.activeId().isEmpty()
                                   ? QSet<QString>{after.activeId()}
                                   : afterLayers;
        history.push(command);
    } catch (const std::exception &e) {
        document = before;
        session.selection = beforeSelection;
        session.selectedLayerIDs = beforeLayers;
        changed();
        emit error(QString::fromUtf8(e.what()));
    }
}
void EditorPage::edit(const QString &label, const std::function<void(Document &)> &operation) {
    canvas->finishTextEditing(true);
    canvas->cancelInteraction();
    auto before = document;
    auto selection = session.selection;
    auto selectedLayers = session.selectedLayerIDs;
    try {
        operation(document);
        if (document.size() != before.size()) {
            session.selection = {};
            session.cropFrame = {};
        }
        record(label, before, document, selection, session.selection, selectedLayers,
               session.selectedLayerIDs);
    } catch (const std::exception &e) {
        document = before;
        session.selection = selection;
        session.selectedLayerIDs = selectedLayers;
        changed();
        emit error(QString::fromUtf8(e.what()));
    }
}
} // namespace compositor
