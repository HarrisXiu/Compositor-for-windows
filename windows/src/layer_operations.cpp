// SPDX-License-Identifier: MIT
#include "layer_operations.h"
#include "render.h"
#include <QHash>
#include <QJsonArray>
#include <QPainter>
#include <algorithm>

namespace compositor {
namespace {
void parentOf(Layer &layer, const QString &parent) {
    if (parent.isEmpty())
        layer.metadata.remove("parentID");
    else
        layer.metadata["parentID"] = parent;
}
QStringList ancestors(const Document &d, const Layer &layer) {
    QStringList result;
    auto parent = layer.parent();
    while (!parent.isEmpty()) {
        result << parent;
        auto node = d.find(parent);
        require(node && result.size() <= 64, "Invalid folder hierarchy");
        parent = node->parent();
    }
    result << QString();
    return result;
}
void releaseDetachedClipping(Document &d) {
    for (auto &layer : d.layers) {
        auto source = d.find(normalizedId(layer.metadata.value("maskSourceID").toString()));
        if (source && source->parent() != layer.parent())
            layer.metadata.remove("maskSourceID");
    }
}
struct MergePlan {
    QSet<QString> included;
    QString anchor, parent, name, label;
};
MergePlan plan(const Document &d, const QSet<QString> &selected) {
    require(d.active(), "Select a layer first");
    MergePlan p;
    if (selected.size() > 1) {
        p.included = layerSubtrees(d, selected);
        for (const auto &layer : d.layers)
            if (selected.contains(layer.id())) {
                p.anchor = layer.id();
                p.parent = layer.parent();
                p.name = layer.name();
            }
        // A selected ancestor is removed; the result belongs to the nearest surviving folder.
        while (p.included.contains(p.parent))
            p.parent = d.find(p.parent)->parent();
        p.label = "Merge Layers";
    } else if (d.active()->group()) {
        p.included = layerSubtrees(d, {d.activeId()});
        p.anchor = d.activeId();
        p.parent = d.active()->parent();
        p.name = d.active()->name();
        p.label = "Merge Group";
    } else {
        auto active = d.active();
        const Layer *below = nullptr;
        for (const auto &layer : d.layers) {
            if (layer.id() == active->id())
                break;
            if (layer.parent() == active->parent())
                below = &layer;
        }
        require(below && !below->group(), "No lower sibling layer to merge");
        p.included = {below->id(), active->id()};
        p.anchor = active->id();
        p.parent = active->parent();
        p.name = below->name();
        p.label = "Merge Down";
    }
    require(std::any_of(d.layers.begin(), d.layers.end(),
                        [&](const Layer &l) { return p.included.contains(l.id()) && !l.group(); }),
            "The selected folders contain no layers");
    return p;
}
QRect alphaBounds(const QImage &image) {
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y))) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
    return right < 0 ? QRect(0, 0, 1, 1) : QRect(QPoint(left, top), QPoint(right, bottom));
}
} // namespace
QStringList layerRoots(const Document &d, const QSet<QString> &selected) {
    QStringList roots;
    for (const auto &layer : d.layers) {
        if (!selected.contains(layer.id()))
            continue;
        auto parents = ancestors(d, layer);
        if (std::none_of(parents.begin(), parents.end(),
                         [&](const QString &id) { return selected.contains(id); }))
            roots << layer.id();
    }
    return roots;
}
QSet<QString> layerSubtrees(const Document &d, const QSet<QString> &selected) {
    QSet<QString> included;
    for (const auto &layer : d.layers)
        if (selected.contains(layer.id()))
            included.insert(layer.id());
    bool changed;
    do {
        changed = false;
        for (const auto &layer : d.layers)
            if (included.contains(layer.parent()) && !included.contains(layer.id())) {
                included.insert(layer.id());
                changed = true;
            }
    } while (changed);
    return included;
}
QString mergeLabel(const Document &d, const QSet<QString> &selected) {
    try {
        return plan(d, selected).label;
    } catch (const Error &) {
        return {};
    }
}
QString mergeLayers(Document &d, const QSet<QString> &selected) {
    const auto p = plan(d, selected);
    Document subset = d;
    subset.layers.clear();
    for (auto layer : d.layers)
        if (p.included.contains(layer.id())) {
            if (!p.included.contains(layer.parent()))
                parentOf(layer, {});
            if (!p.included.contains(normalizedId(layer.metadata.value("maskSourceID").toString())))
                layer.metadata.remove("maskSourceID");
            subset.layers << layer;
        }
    require(subset.previewLimitations().isEmpty(),
            "Unsupported effects must be migrated before merging");
    auto raster = renderDocument(subset);
    const auto bounds = alphaBounds(raster);
    Document asset = Document::create(d.size());
    const auto id = asset.addImage(p.name, raster.copy(bounds));
    auto merged = *asset.find(id);
    merged.setBounds(bounds);
    parentOf(merged, p.parent);
    QVector<Layer> next;
    for (auto layer : d.layers) {
        if (layer.id() == p.anchor)
            next << merged;
        if (p.included.contains(layer.id()))
            continue;
        if (p.included.contains(normalizedId(layer.metadata.value("maskSourceID").toString())))
            layer.metadata["maskSourceID"] = id;
        next << layer;
    }
    d.layers = next;
    d.metadata["activeLayerID"] = id;
    d.metadata["version"] = CurrentVersion;
    return id;
}
QString groupLayers(Document &d, const QSet<QString> &selected) {
    const auto roots = layerRoots(d, selected);
    require(!roots.isEmpty(), "Select layers to group");
    QString common;
    for (const auto &candidate : ancestors(d, *d.find(roots.front())))
        if (std::all_of(roots.begin(), roots.end(), [&](const QString &id) {
                return ancestors(d, *d.find(id)).contains(candidate);
            })) {
            common = candidate;
            break;
        }
    QSet<QString> branches;
    for (auto id : roots) {
        while (d.find(id)->parent() != common)
            id = d.find(id)->parent();
        branches.insert(id);
    }
    int insertion = 0;
    for (int i = 0; i < d.layers.size(); ++i)
        if (branches.contains(d.layers[i].id()))
            insertion = i + 1;
    QString name;
    int number = 1;
    do {
        name = "Folder " + QString::number(number++);
    } while (std::any_of(d.layers.begin(), d.layers.end(),
                         [&](const Layer &l) { return l.name() == name; }));
    const auto id = d.addGroup(name);
    auto folder = d.layers.takeLast();
    parentOf(folder, common);
    d.layers.insert(insertion, folder);
    for (const auto &root : roots)
        parentOf(*d.find(root), id);
    releaseDetachedClipping(d);
    return id;
}
QStringList ungroupLayer(Document &d, const QString &id) {
    const auto folder = d.find(id);
    require(folder && folder->group(), "Select a folder to ungroup");
    const auto parent = folder->parent();
    QVector<Layer> children, next;
    QStringList selected;
    for (auto layer : d.layers)
        if (layer.parent() == id) {
            parentOf(layer, parent);
            children << layer;
            selected << layer.id();
        }
    for (const auto &layer : d.layers) {
        if (layer.id() == id)
            next.append(children);
        else if (layer.parent() != id)
            next << layer;
    }
    d.layers = next;
    d.metadata["activeLayerID"] = selected.isEmpty() ? QString() : selected.back();
    releaseDetachedClipping(d);
    return selected;
}
void moveLayers(Document &d, const QSet<QString> &selected, int direction) {
    const auto roots = layerRoots(d, selected);
    QSet<QString> picked(roots.begin(), roots.end());
    QSet<QString> parents;
    for (const auto &id : roots)
        parents.insert(d.find(id)->parent());
    for (const auto &parent : parents) {
        QVector<int> indices;
        for (int i = 0; i < d.layers.size(); ++i)
            if (d.layers[i].parent() == parent)
                indices << i;
        if (direction > 0) {
            for (int i = indices.size() - 2; i >= 0; --i)
                if (picked.contains(d.layers[indices[i]].id()) &&
                    !picked.contains(d.layers[indices[i + 1]].id()))
                    std::swap(d.layers[indices[i]], d.layers[indices[i + 1]]);
        } else {
            for (int i = 1; i < indices.size(); ++i)
                if (picked.contains(d.layers[indices[i]].id()) &&
                    !picked.contains(d.layers[indices[i - 1]].id()))
                    std::swap(d.layers[indices[i]], d.layers[indices[i - 1]]);
        }
    }
    releaseDetachedClipping(d);
}
void moveLayersOut(Document &d, const QSet<QString> &selected) {
    const auto roots = layerRoots(d, selected);
    for (auto it = roots.crbegin(); it != roots.crend(); ++it) {
        const auto &id = *it;
        auto layer = d.find(id);
        if (layer->parent().isEmpty())
            continue;
        const auto folder = *d.find(layer->parent());
        auto moved = *layer;
        parentOf(moved, folder.parent());
        d.layers.erase(std::remove_if(d.layers.begin(), d.layers.end(),
                                      [&](const Layer &l) { return l.id() == id; }),
                       d.layers.end());
        auto anchor = std::find_if(d.layers.begin(), d.layers.end(),
                                   [&](const Layer &l) { return l.id() == folder.id(); });
        d.layers.insert(anchor + 1, moved);
    }
    releaseDetachedClipping(d);
}
void moveLayersTo(Document &d, const QSet<QString> &selected, const QString &parent,
                  const QString &anchor, bool above) {
    const auto roots = layerRoots(d, selected);
    const auto included = layerSubtrees(d, selected);
    require(!roots.isEmpty(), "Select layers to move");
    require(parent.isEmpty() || (d.find(parent) && d.find(parent)->group()),
            "Drop layers onto a folder");
    require(!included.contains(parent), "A folder cannot contain itself");
    if (roots.contains(anchor))
        return;
    require(anchor.isEmpty() || (d.find(anchor) && d.find(anchor)->parent() == parent),
            "Invalid layer drop position");
    QVector<Layer> moved;
    for (const auto &id : roots) {
        auto layer = *d.find(id);
        parentOf(layer, parent);
        moved << layer;
    }
    d.layers.erase(std::remove_if(d.layers.begin(), d.layers.end(),
                                  [&](const Layer &layer) { return roots.contains(layer.id()); }),
                   d.layers.end());
    auto insertion = d.layers.end();
    if (!anchor.isEmpty()) {
        insertion = std::find_if(d.layers.begin(), d.layers.end(),
                                 [&](const Layer &layer) { return layer.id() == anchor; });
        if (above)
            ++insertion;
    }
    const auto offset = insertion - d.layers.begin();
    for (int i = 0; i < moved.size(); ++i)
        d.layers.insert(offset + i, moved[i]);
    releaseDetachedClipping(d);
    d.validateAssets();
}
QStringList copyLayers(Document &destination, const Document &source, const QSet<QString> &selected,
                       std::optional<QPointF> center, bool duplicate) {
    const auto roots = layerRoots(source, selected);
    require(!roots.isEmpty(), "Select layers to copy");
    const auto included = layerSubtrees(source, selected);
    qint64 pixels = 0, masks = 0;
    for (const auto &layer : destination.layers) {
        pixels += qint64(layer.image.width()) * layer.image.height();
        masks += qint64(layer.mask.width()) * layer.mask.height();
    }
    for (const auto &layer : source.layers)
        if (included.contains(layer.id())) {
            pixels += qint64(layer.image.width()) * layer.image.height();
            masks += qint64(layer.mask.width()) * layer.mask.height();
        }
    require(pixels <= documentPixelBudget() && masks <= documentPixelBudget(),
            "Copied layers exceed memory limit");
    QHash<QString, QString> mapping;
    for (const auto &id : included)
        mapping[id] = newId();
    QRectF box;
    for (const auto &layer : source.layers)
        if (included.contains(layer.id()) && !layer.group()) {
            const auto origin = layer.transform().value("origin").toArray();
            const auto size = layer.transform().value("size").toArray();
            const QRectF bounds(origin[0].toDouble(), origin[1].toDouble(), size[0].toDouble(),
                                size[1].toDouble());
            box = box.isEmpty() ? bounds : box.united(bounds);
        }
    if (roots.size() == 1 || box.isEmpty()) {
        auto layer = source.find(roots.front());
        auto origin = layer->transform().value("origin").toArray();
        auto size = layer->transform().value("size").toArray();
        box = QRectF(origin[0].toDouble(), origin[1].toDouble(), size[0].toDouble(),
                     size[1].toDouble());
    }
    const QPointF delta = duplicate ? QPointF()
                                    : center.value_or(QPointF(destination.size().width() / 2.0,
                                                              destination.size().height() / 2.0)) -
                                          box.center();
    QVector<Layer> copies;
    for (auto layer : source.layers) {
        if (!included.contains(layer.id()))
            continue;
        const auto oldId = layer.id();
        const auto clip = normalizedId(layer.metadata.value("maskSourceID").toString());
        if (!clip.isEmpty() && !included.contains(clip) &&
            (!duplicate || !destination.find(clip))) {
            if (!layer.image.isNull()) {
                layer.image = bakeClipping(source, layer);
                layer.metadata.remove("text");
                layer.metadata.remove("shape");
            }
            layer.metadata.remove("maskSourceID");
        } else if (mapping.contains(clip))
            layer.metadata["maskSourceID"] = mapping.value(clip);
        const auto id = mapping.value(oldId);
        layer.metadata["id"] = id;
        if (!layer.image.isNull())
            layer.metadata["imageFile"] = id + ".png";
        if (!layer.mask.isNull())
            layer.metadata["maskFile"] = id + ".mask.png";
        if (mapping.contains(layer.parent()))
            parentOf(layer, mapping.value(layer.parent()));
        else if (!duplicate || !destination.find(layer.parent()))
            parentOf(layer, {});
        auto maskPlacement = layer.metadata.value("maskPlacement").toObject();
        // Whole-document transfers move placed masks whether they are linked or not.
        layer.move(delta);
        if (!maskPlacement.isEmpty()) {
            auto origin = maskPlacement.value("origin").toArray();
            maskPlacement["origin"] =
                QJsonArray{origin[0].toDouble() + delta.x(), origin[1].toDouble() + delta.y()};
            layer.metadata["maskPlacement"] = maskPlacement;
        }
        copies << layer;
    }
    destination.layers.append(copies);
    destination.metadata["version"] = CurrentVersion;
    QStringList result;
    for (const auto &id : roots)
        result << mapping.value(id);
    destination.metadata["activeLayerID"] = result.back();
    destination.validateAssets();
    return result;
}
QImage layerAlphaSelection(const Document &d, const Layer &layer, bool mask) {
    QImage asset = mask ? layer.mask : layer.image;
    Layer placement = layer;
    if (!mask && asset.isNull()) {
        Document subset = d;
        subset.layers.clear();
        const auto included = layerSubtrees(d, {layer.id()});
        for (auto child : d.layers)
            if (included.contains(child.id())) {
                if (!included.contains(child.parent()))
                    parentOf(child, {});
                if (!included.contains(
                        normalizedId(child.metadata.value("maskSourceID").toString())))
                    child.metadata.remove("maskSourceID");
                subset.layers << child;
            }
        require(subset.previewLimitations().isEmpty(),
                "Unsupported effects must be migrated before loading transparency");
        asset = renderDocument(subset);
        placement.metadata["transform"] = makeTransform(QRect(QPoint(), d.size()));
    }
    require(!asset.isNull(), mask ? "Select a layer with a mask" : "Select a pixel layer");
    QImage alpha(asset.size(), QImage::Format_ARGB32_Premultiplied);
    require(!alpha.isNull(), "Not enough memory for selection");
    for (int y = 0; y < alpha.height(); ++y) {
        auto output = reinterpret_cast<QRgb *>(alpha.scanLine(y));
        for (int x = 0; x < alpha.width(); ++x) {
            const int value = mask ? asset.constScanLine(y)[x] : qAlpha(asset.pixel(x, y));
            output[x] = qRgba(value, value, value, 255);
        }
    }
    if (mask && layer.metadata.value("maskPlacement").isObject())
        placement.metadata["transform"] = layer.metadata.value("maskPlacement");
    QImage result(d.size(), QImage::Format_Grayscale8);
    require(!result.isNull(), "Not enough memory for selection");
    result.fill(0);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          placement.transform().value("sampling") != "Nearest");
    painter.setTransform(placement.placement(asset.size()));
    painter.drawImage(0, 0, alpha);
    return result;
}
} // namespace compositor
