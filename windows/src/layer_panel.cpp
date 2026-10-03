// SPDX-License-Identifier: MIT
#include "editor.h"
#include "language.h"
#include "layer_operations.h"
#include "layer_transfer.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDropEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace compositor {
class LayerTree final : public QTreeWidget {
  public:
    std::function<void(QSet<QString>, QString, QString, bool)> relocated;
    std::function<Document()> document;
    std::function<QSet<QString>()> selection;
    std::function<void(QString, bool, Qt::KeyboardModifiers)> thumbnailClicked;

  protected:
    void startDrag(Qt::DropActions) override {
        if (!document || selection().isEmpty())
            return;
        QDrag drag(this);
        drag.setMimeData(mimeData(selectedItems()));
        // Each target commits a document move or cross-project copy atomically.
        drag.exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
    }
    void dropEvent(QDropEvent *event) override {
        const auto transfer = layerTransfer(event->mimeData());
        if (!transfer || transfer->source.metadata.value("documentID") !=
                             document().metadata.value("documentID")) {
            event->ignore();
            return;
        }
        auto item = itemAt(event->position().toPoint());
        QString parent, anchor;
        bool above = false;
        const auto position = dropIndicatorPosition();
        if (item && position == OnItem) {
            const auto id = item->data(0, Qt::UserRole).toString();
            auto snapshot = document();
            auto layer = snapshot.find(id);
            if (!layer || !layer->group()) {
                event->ignore();
                return;
            }
            parent = id;
        } else if (item && position != OnViewport) {
            anchor = item->data(0, Qt::UserRole).toString();
            parent = item->parent() ? item->parent()->data(0, Qt::UserRole).toString() : QString();
            above = position == AboveItem;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
        relocated(transfer->selected, parent, anchor, above);
    }
    void mousePressEvent(QMouseEvent *event) override {
        auto item = itemAt(event->position().toPoint());
        const int column = columnAt(int(event->position().x()));
        QStyleOptionViewItem option;
        option.initFrom(this);
        if (item) {
            option.rect = visualItemRect(item);
            option.features =
                QStyleOptionViewItem::HasCheckIndicator | QStyleOptionViewItem::HasDecoration;
            option.decorationSize = iconSize();
            option.decorationPosition = QStyleOptionViewItem::Left;
        }
        const auto iconRect =
            style()->subElementRect(QStyle::SE_ItemViewItemDecoration, &option, this);
        const bool thumbnail =
            item && (column == 1 || (column == 0 && !item->icon(0).isNull() &&
                                     iconRect.contains(event->position().toPoint())));
        if (item && thumbnail && event->button() == Qt::LeftButton &&
            (column == 0 || !item->icon(1).isNull())) {
            if (event->modifiers() & Qt::ControlModifier) {
                thumbnailClicked(item->data(0, Qt::UserRole).toString(), column == 1,
                                 event->modifiers());
                return;
            }
            const auto id = item->data(0, Qt::UserRole).toString();
            QTreeWidget::mousePressEvent(event);
            thumbnailClicked(id, column == 1, event->modifiers());
            return;
        }
        QTreeWidget::mousePressEvent(event);
    }
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override {
        auto native = std::unique_ptr<QMimeData>(QTreeWidget::mimeData(items));
        auto result = new LayerTransferMimeData(document(), selection());
        if (native)
            for (const auto &format : native->formats())
                result->setData(format, native->data(format));
        return result;
    }
    QStringList mimeTypes() const override {
        auto result = QTreeWidget::mimeTypes();
        result << "application/x-compositor-layer-transfer";
        return result;
    }
    Qt::DropActions supportedDropActions() const override {
        return Qt::MoveAction | Qt::CopyAction;
    }
};
void EditorWindow::buildPanels() {
    buildToolOptions();
    auto dock = new QDockWidget("Layers / Transform", this);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto contents = new QWidget;
    auto layout = new QVBoxLayout(contents);
    limitations_ = new QLabel;
    limitations_->setWordWrap(true);
    limitations_->setStyleSheet("color:#eab676;padding:6px;");
    layout->addWidget(limitations_);
    blend_ = new QComboBox;
    blend_->setObjectName("layerBlendMode");
    blend_->addItems(blendModes());
    layout->addWidget(blend_);
    opacity_ = new QDoubleSpinBox;
    opacity_->setRange(0, 100);
    opacity_->setSuffix(" % opacity");
    layout->addWidget(opacity_);
    auto layerTree = new LayerTree;
    layerTree->relocated = [this](const QSet<QString> &ids, const QString &parent,
                                  const QString &anchor, bool above) {
        if (auto p = page())
            p->edit("Reorder Layers",
                    [&](Document &d) { moveLayersTo(d, ids, parent, anchor, above); });
    };
    layerTree->document = [this] { return page()->document; };
    layerTree->selection = [this] { return selectedLayers(); };
    layerTree->thumbnailClicked = [this](const QString &id, bool mask,
                                         Qt::KeyboardModifiers modifiers) {
        auto p = page();
        auto layer = p ? p->document.find(id) : nullptr;
        if (!layer)
            return;
        if (modifiers & Qt::ControlModifier) {
            try {
                auto alpha = layerAlphaSelection(p->document, *layer, mask);
                const auto before = p->session.selection;
                if (!before.isNull() && (modifiers & (Qt::ShiftModifier | Qt::AltModifier)))
                    for (int y = 0; y < alpha.height(); ++y)
                        for (int x = 0; x < alpha.width(); ++x) {
                            auto &pixel = alpha.scanLine(y)[x];
                            pixel = modifiers & Qt::AltModifier
                                        ? uchar(before.constScanLine(y)[x] * (1 - pixel / 255.0))
                                        : std::max(pixel, before.constScanLine(y)[x]);
                        }
                p->canvas->replaceSelection(alpha,
                                            mask ? "Mask to Selection" : "Layer to Selection");
            } catch (const std::exception &error) {
                showError(QString::fromUtf8(error.what()));
            }
        } else {
            p->session.target = mask ? EditTarget::Mask : EditTarget::Pixels;
            QTimer::singleShot(0, this, &EditorWindow::refreshPanels);
        }
    };
    layers_ = layerTree;
    layers_->setObjectName("layerTree");
    layers_->setColumnCount(2);
    layers_->setIconSize({44, 36});
    layers_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layers_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    layers_->setColumnWidth(1, 52);
    layers_->setHeaderHidden(true);
    layers_->setDragDropMode(QAbstractItemView::DragDrop);
    layers_->setDefaultDropAction(Qt::MoveAction);
    layers_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layers_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(layers_, &QTreeWidget::customContextMenuRequested, this, [this](QPoint position) {
        if (!page())
            return;
        if (auto item = layers_->itemAt(position)) {
            if (!item->isSelected()) {
                layers_->clearSelection();
                item->setSelected(true);
            }
            layers_->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            page()->document.metadata["activeLayerID"] = item->data(0, Qt::UserRole).toString();
        }
        QMenu menu(this);
        for (auto key :
             {"Duplicate Layer", "Delete Layer", "Group Layers", "Ungroup Layers", "Merge Down",
              "Move Layer Up", "Move Layer Down", "Move Out of Folder", "Copy Layers",
              "Paste Image", "Add White Mask", "Invert Mask", "Toggle Mask", "Link / Unlink Mask",
              "Remove Mask", "Edit Text…", "Edit Shape…", "Edit Adjustment…"}) {
            for (auto action : findChildren<QAction *>())
                if (action->property("layerAction").toString() == QLatin1String(key) ||
                    action->property("_uiSource_text").toString() == QLatin1String(key)) {
                    menu.addAction(action);
                    break;
                }
        }
        if (mergeAction_) {
            const auto label = mergeLabel(page()->document, selectedLayers());
            mergeAction_->setEnabled(!label.isEmpty());
            mergeAction_->setProperty("_uiSource_text", label.isEmpty() ? "Merge Down" : label);
            mergeAction_->setText(label.isEmpty() ? "Merge Down" : label);
        }
        menu.exec(layers_->viewport()->mapToGlobal(position));
    });
    layers_->setMinimumWidth(255);
    layout->addWidget(layers_, 1);
    auto buttons = new QHBoxLayout;
    for (auto text : {"+ Layer", "+ Folder", "+ Mask"}) {
        auto button = new QPushButton(text);
        buttons->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, label = QString(text)] {
            if (!page())
                return;
            if (label == "+ Mask")
                addMask();
            else
                page()->edit(label, [&](Document &d) {
                    if (label == "+ Layer")
                        d.addBlank("Layer");
                    else
                        d.addGroup("Folder");
                });
        });
    }
    layout->addLayout(buttons);
    auto form = new QFormLayout;
    auto spin = [&](const QString &label, double min, double max) {
        auto s = new QDoubleSpinBox;
        s->setRange(min, max);
        s->setDecimals(2);
        form->addRow(label, s);
        connect(s, &QDoubleSpinBox::editingFinished, this, &EditorWindow::updateTransform);
        return s;
    };
    x_ = spin("X", -1000000, 1000000);
    y_ = spin("Y", -1000000, 1000000);
    width_ = spin("Width", 1, 300000);
    height_ = spin("Height", 1, 300000);
    angle_ = spin("Rotation", -36000, 36000);
    layout->addLayout(form);
    dock->setWidget(contents);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    connect(blend_, &QComboBox::currentTextChanged, this, [this](const QString &) {
        auto value = comboValue(blend_);
        if (!syncing_ && page())
            page()->edit("Blend Mode", [&](Document &d) {
                for (const auto &id : selectedLayers())
                    if (auto l = d.find(id); l && !l->group())
                        l->metadata["blendMode"] = value;
                d.metadata["version"] = CurrentVersion;
            });
    });
    connect(opacity_, &QDoubleSpinBox::editingFinished, this, [this] {
        if (!syncing_ && page())
            page()->edit("Layer Opacity", [&](Document &d) {
                for (const auto &id : selectedLayers())
                    if (auto l = d.find(id))
                        l->metadata["opacity"] = opacity_->value() / 100;
                d.metadata["version"] = CurrentVersion;
            });
    });
    connect(layers_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (syncing_ || !page() || !item)
            return;
        page()->canvas->cancelInteraction();
        page()->document.metadata["activeLayerID"] = item->data(0, Qt::UserRole).toString();
        if (auto layer = page()->document.active(); !layer || layer->mask.isNull())
            page()->session.target = EditTarget::Pixels;
        QTimer::singleShot(0, this, &EditorWindow::refreshPanels);
    });
    connect(layers_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (syncing_ || !page())
            return;
        QSet<QString> ids;
        for (auto item : layers_->selectedItems())
            ids.insert(item->data(0, Qt::UserRole).toString());
        page()->session.selectedLayerIDs = ids;
        if (!ids.contains(page()->document.activeId()))
            page()->document.metadata["activeLayerID"] = ids.isEmpty() ? QString() : *ids.begin();
        QTimer::singleShot(0, this, &EditorWindow::refreshPanels);
    });
    connect(layers_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (!syncing_ && page())
            page()->session.collapsedLayerIDs.insert(item->data(0, Qt::UserRole).toString());
    });
    connect(layers_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (!syncing_ && page())
            page()->session.collapsedLayerIDs.remove(item->data(0, Qt::UserRole).toString());
    });
    connect(layers_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (syncing_ || !page())
            return;
        auto id = item->data(0, Qt::UserRole).toString(), name = item->text(0);
        bool visible = item->checkState(0) == Qt::Checked;
        auto p = page();
        // Rebuilding the tree inside a model's dataChanged signal invalidates its item.
        QTimer::singleShot(0, p, [p, id, name, visible] {
            p->edit("Layer Properties", [&](Document &d) {
                if (auto l = d.find(id)) {
                    l->metadata["name"] = name;
                    l->metadata["isVisible"] = visible;
                }
            });
        });
    });
}
void EditorWindow::refreshPanels() {
    if (syncing_)
        return;
    syncing_ = true;
    const auto scroll = layers_->verticalScrollBar()->value();
    layers_->clear();
    auto p = page();
    for (int i = 0; i < tabs_->count(); ++i) {
        auto tab = qobject_cast<EditorPage *>(tabs_->widget(i));
        if (tab)
            tabs_->setTabText(
                i, (tab->path.isEmpty() ? uiText("Untitled") : QFileInfo(tab->path).fileName()) +
                       (tab->isModified() ? " *" : ""));
    }
    if (!p) {
        limitations_->setText("Create or open a project");
        syncing_ = false;
        return;
    }
    auto unsupported = p->document.previewLimitations();
    limitations_->setText(unsupported.isEmpty()
                              ? ""
                              : "Preview pending: " + unsupported.join(", ") +
                                    ". Metadata is preserved; export is blocked.");
    std::function<void(const QString &, QTreeWidgetItem *)> add = [&](const QString &parent,
                                                                      QTreeWidgetItem *item) {
        for (auto it = p->document.layers.crbegin(); it != p->document.layers.crend(); ++it) {
            if (it->parent() != parent)
                continue;
            auto next = item ? new QTreeWidgetItem(item) : new QTreeWidgetItem(layers_);
            next->setText(0, it->name());
            next->setData(0, Qt::UserRole, it->id());
            next->setFlags(next->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable |
                           Qt::ItemIsDragEnabled);
            if (!it->group())
                next->setFlags(next->flags() & ~Qt::ItemIsDropEnabled);
            next->setCheckState(0, it->visible() ? Qt::Checked : Qt::Unchecked);
            if (!it->image.isNull())
                next->setIcon(0, QIcon(QPixmap::fromImage(it->image.scaled(
                                     40, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
            else
                next->setIcon(0, style()->standardIcon(it->group() ? QStyle::SP_DirIcon
                                                                   : QStyle::SP_FileIcon));
            if (!it->mask.isNull()) {
                next->setIcon(1, QIcon(QPixmap::fromImage(it->mask.scaled(
                                     40, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
                next->setToolTip(1, uiText("Click to edit mask; Ctrl-click to load selection"));
                next->setBackground(1, it->id() == p->document.activeId() &&
                                               p->session.target == EditTarget::Mask
                                           ? QColor("#446688")
                                           : QColor("#242424"));
            }
            next->setToolTip(0, uiText("Ctrl-click thumbnail to load layer transparency"));
            if (it->group()) {
                add(it->id(), next);
                next->setExpanded(!p->session.collapsedLayerIDs.contains(it->id()));
            }
            next->setSelected(
                p->session.selectedLayerIDs.contains(it->id()) ||
                (p->session.selectedLayerIDs.isEmpty() && it->id() == p->document.activeId()));
            if (it->id() == p->document.activeId())
                layers_->setCurrentItem(next, 0, QItemSelectionModel::NoUpdate);
        }
    };
    add({}, nullptr);
    if (auto l = p->document.active()) {
        selectComboValue(blend_, l->blend());
        blend_->setEnabled(!l->group());
        opacity_->setValue(l->opacity() * 100);
        auto t = l->transform();
        auto o = t.value("origin").toArray(), s = t.value("size").toArray();
        x_->setValue(o[0].toDouble());
        y_->setValue(o[1].toDouble());
        width_->setValue(s[0].toDouble());
        height_->setValue(s[1].toDouble());
        angle_->setValue(t.value("rotation").toDouble());
        maskTarget_->setEnabled(!l->mask.isNull());
    } else {
        blend_->setEnabled(false);
        maskTarget_->setEnabled(false);
    }
    syncToolOptions();
    if (mergeAction_) {
        const auto label = mergeLabel(p->document, selectedLayers());
        mergeAction_->setEnabled(!label.isEmpty());
        mergeAction_->setProperty("_uiSource_text", label.isEmpty() ? "Merge Down" : label);
        mergeAction_->setText(label.isEmpty() ? "Merge Down" : label);
    }
    syncing_ = false;
    layers_->verticalScrollBar()->setValue(scroll);
}
void EditorWindow::rebuildLayerOrder() {
    if (syncing_ || !page())
        return;
    QVector<QPair<QString, QString>> order;
    std::function<void(QTreeWidgetItem *, const QString &)> walk = [&](QTreeWidgetItem *node,
                                                                       const QString &parent) {
        int count = node ? node->childCount() : layers_->topLevelItemCount();
        for (int i = count - 1; i >= 0; --i) {
            auto item = node ? node->child(i) : layers_->topLevelItem(i);
            auto id = item->data(0, Qt::UserRole).toString();
            order.push_back({id, parent});
            walk(item, id);
        }
    };
    walk(nullptr, {});
    page()->edit("Reorder Layers", [&](Document &d) {
        QVector<Layer> next;
        for (const auto &entry : order) {
            auto l = d.find(entry.first);
            require(l, "Missing layer in tree");
            auto copy = *l;
            if (entry.second.isEmpty())
                copy.metadata.remove("parentID");
            else
                copy.metadata["parentID"] = entry.second;
            next.push_back(copy);
        }
        d.layers = next;
        // A drag that separates a clipping layer from its base releases that link.
        for (auto &layer : d.layers) {
            auto base = d.find(normalizedId(layer.metadata.value("maskSourceID").toString()));
            if (base && base->parent() != layer.parent())
                layer.metadata.remove("maskSourceID");
        }
        d.metadata["version"] = CurrentVersion;
    });
}
void EditorWindow::updateTransform() {
    if (syncing_ || !page())
        return;
    const double x = x_->value(), y = y_->value(), w = width_->value(), h = height_->value(),
                 a = angle_->value();
    page()->edit("Transform Layer", [&](Document &d) {
        if (auto l = d.active()) {
            l->setBounds({x, y, w, h});
            auto t = l->transform();
            t["rotation"] = a;
            l->metadata["transform"] = t;
        }
    });
}
} // namespace compositor
