// SPDX-License-Identifier: MIT
#include "editor.h"
#include "layer_operations.h"
#include "layer_transfer.h"
#include "shortcuts.h"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>

namespace compositor {
QSet<QString> EditorWindow::selectedLayers() const {
    auto p = page();
    if (!p)
        return {};
    auto ids = p->session.selectedLayerIDs;
    if (ids.isEmpty() && !p->document.activeId().isEmpty())
        ids.insert(p->document.activeId());
    return ids;
}
void EditorWindow::mergeSelectedLayers() {
    auto p = page();
    if (!p)
        return;
    const auto ids = selectedLayers();
    const auto label = mergeLabel(p->document, ids);
    require(!label.isEmpty(), "No layers to merge");
    p->edit(label, [&](Document &d) { p->session.selectedLayerIDs = {mergeLayers(d, ids)}; });
}
void EditorWindow::groupSelectedLayers() {
    if (auto p = page()) {
        auto ids = selectedLayers();
        p->edit("Group Layers",
                [&](Document &d) { p->session.selectedLayerIDs = {groupLayers(d, ids)}; });
    }
}
void EditorWindow::ungroupSelectedLayer() {
    if (auto p = page())
        p->edit("Ungroup Layers", [&](Document &d) {
            auto ids = ungroupLayer(d, d.activeId());
            p->session.selectedLayerIDs = QSet<QString>(ids.begin(), ids.end());
        });
}
void EditorWindow::duplicateSelectedLayers() {
    if (auto p = page()) {
        const auto source = p->document;
        const auto selected = selectedLayers();
        p->edit("Duplicate Layers", [&](Document &d) {
            const auto ids = copyLayers(d, source, selected, std::nullopt, true);
            p->session.selectedLayerIDs = QSet<QString>(ids.begin(), ids.end());
        });
    }
}
void EditorWindow::deleteSelectedLayers() {
    if (auto p = page()) {
        const auto roots = layerRoots(p->document, selectedLayers());
        p->edit("Delete Layers", [&](Document &d) {
            for (const auto &id : roots)
                d.remove(id);
            p->session.selectedLayerIDs.clear();
        });
    }
}
void EditorWindow::copySelectedLayers() {
    auto p = page();
    require(p && !selectedLayers().isEmpty(), "Select layers to copy");
    p->canvas->finishTextEditing(true);
    p->canvas->commitFloatingSelection();
    p->canvas->cancelInteraction();
    auto mime = new LayerTransferMimeData(p->document, selectedLayers());
    QApplication::clipboard()->setMimeData(mime);
}
bool EditorWindow::pasteCopiedLayers() {
    auto transfer = layerTransfer(QApplication::clipboard()->mimeData());
    if (!transfer)
        return false;
    if (!page())
        addPage(Document::create(transfer->source.size()));
    auto p = page();
    const bool same =
        p->document.metadata.value("documentID") == transfer->source.metadata.value("documentID");
    p->edit("Paste Layers", [&](Document &d) {
        const auto ids = copyLayers(d, transfer->source, transfer->selected, std::nullopt, same);
        p->session.selectedLayerIDs = QSet<QString>(ids.begin(), ids.end());
    });
    return true;
}
bool EditorWindow::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::ShortcutOverride) {
        auto widget = qobject_cast<QWidget *>(object);
        if (widget && isAncestorOf(widget) &&
            (qobject_cast<QLineEdit *>(widget) || qobject_cast<QTextEdit *>(widget) ||
             qobject_cast<QPlainTextEdit *>(widget) || qobject_cast<QAbstractSpinBox *>(widget) ||
             (qobject_cast<QComboBox *>(widget) &&
              qobject_cast<QComboBox *>(widget)->isEditable()))) {
            auto key = static_cast<QKeyEvent *>(event);
            if (!(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
                event->accept();
                return true;
            }
            for (const auto &entry : Shortcuts::instance().entries())
                if (entry.group == "Canvas" || entry.group == "Tools") {
                    const auto sequence = Shortcuts::instance().sequence(entry);
                    if (!sequence.isEmpty() && sequence[0] == key->keyCombination()) {
                        event->accept();
                        return true;
                    }
                }
        }
    }
    auto bar = qobject_cast<QTabBar *>(object);
    if (bar && bar->parentWidget() == tabs_ &&
        (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
         event->type() == QEvent::Drop)) {
        auto drop = static_cast<QDropEvent *>(event);
        auto transfer = layerTransfer(drop->mimeData());
        if (!transfer)
            return QMainWindow::eventFilter(object, event);
        const int index = bar->tabAt(drop->position().toPoint());
        auto target = index >= 0 ? qobject_cast<EditorPage *>(tabs_->widget(index)) : nullptr;
        if (!target || target->document.metadata.value("documentID") ==
                           transfer->source.metadata.value("documentID")) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        if (event->type() == QEvent::Drop) {
            target->edit("Copy Layers from Project", [&](Document &d) {
                const auto ids = copyLayers(d, transfer->source, transfer->selected);
                target->session.selectedLayerIDs = QSet<QString>(ids.begin(), ids.end());
            });
            tabs_->setCurrentIndex(index);
        }
        return true;
    }
    return QMainWindow::eventFilter(object, event);
}
} // namespace compositor
