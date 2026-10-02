// SPDX-License-Identifier: MIT
#include "editor.h"
#include "language.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace compositor {
class LayerTree final : public QTreeWidget {
  public:
    std::function<void()> moved;

  protected:
    void dropEvent(QDropEvent *event) override {
        QTreeWidget::dropEvent(event);
        if (event->isAccepted() && moved)
            QTimer::singleShot(0, this, moved);
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
    blend_->addItems(blendModes());
    layout->addWidget(blend_);
    opacity_ = new QDoubleSpinBox;
    opacity_->setRange(0, 100);
    opacity_->setSuffix(" % opacity");
    layout->addWidget(opacity_);
    auto layerTree = new LayerTree;
    layerTree->moved = [this] { rebuildLayerOrder(); };
    layers_ = layerTree;
    layers_->setHeaderHidden(true);
    layers_->setDragDropMode(QAbstractItemView::InternalMove);
    layers_->setDefaultDropAction(Qt::MoveAction);
    layers_->setSelectionMode(QAbstractItemView::SingleSelection);
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
                if (auto l = d.active()) {
                    require(!l->group(), "Folders use Normal blending");
                    l->metadata["blendMode"] = value;
                    d.metadata["version"] = CurrentVersion;
                }
            });
    });
    connect(opacity_, &QDoubleSpinBox::editingFinished, this, [this] {
        if (!syncing_ && page())
            page()->edit("Layer Opacity", [&](Document &d) {
                if (auto l = d.active()) {
                    l->metadata["opacity"] = opacity_->value() / 100;
                    d.metadata["version"] = CurrentVersion;
                }
            });
    });
    connect(layers_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (syncing_ || !page() || !item)
            return;
        page()->canvas->cancelInteraction();
        page()->document.metadata["activeLayerID"] = item->data(0, Qt::UserRole).toString();
        if (auto layer = page()->document.active(); !layer || layer->mask.isNull())
            page()->session.target = EditTarget::Pixels;
        refreshPanels();
    });
    connect(layers_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (syncing_ || !page())
            return;
        auto id = item->data(0, Qt::UserRole).toString(), name = item->text(0);
        bool visible = item->checkState(0) == Qt::Checked;
        page()->edit("Layer Properties", [&](Document &d) {
            if (auto l = d.find(id)) {
                l->metadata["name"] = name;
                l->metadata["isVisible"] = visible;
            }
        });
    });
}
void EditorWindow::refreshPanels() {
    if (syncing_)
        return;
    syncing_ = true;
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
            if (it->group()) {
                add(it->id(), next);
                next->setExpanded(true);
            }
            if (it->id() == p->document.activeId())
                layers_->setCurrentItem(next);
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
    syncing_ = false;
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
