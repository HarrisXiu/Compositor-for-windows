// SPDX-License-Identifier: MIT
#include "canvas.h"
#include "canvas_layout.h"
#include "distort.h"
#include "layer_operations.h"
#include <QJsonArray>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace compositor {
namespace {
QPointF pairOf(const QJsonObject &o, const char *key) {
    const auto a = o.value(QLatin1String(key)).toArray();
    return {a.at(0).toDouble(), a.at(1).toDouble()};
}
bool shown(const Document &document, const Layer &layer) {
    if (!layer.visible())
        return false;
    for (auto parent = document.find(layer.parent()); parent;
         parent = document.find(parent->parent()))
        if (!parent->visible())
            return false;
    return true;
}
// A corner of the box moves the handle's corner, an edge handle both corners of that edge.
QVector<int> distortedCorners(int handle) {
    return handle % 2 == 0 ? QVector<int>{handle / 2}
                           : QVector<int>{handle / 2, (handle / 2 + 1) % 4};
}
} // namespace

Canvas::TransformSubject Canvas::transformSubject() const {
    TransformSubject subject;
    const auto *active = document_->active();
    if (!active)
        return subject;
    const auto &selected = session_->selectedLayerIDs;
    if (paintMask() && !active->group() && !active->mask.isNull()) {
        subject.kind = TransformSubject::Kind::Mask;
        subject.ids = {active->id()};
        subject.box = transformTarget(*active).transform();
        return subject;
    }
    // Several layers, or a folder, transform together in one box around what they draw.
    if (selected.size() > 1 || (selected.size() == 1 && active->group())) {
        const auto included = layerSubtrees(*document_, selected);
        QVector<QJsonObject> boxes, visible;
        for (const auto &layer : document_->layers) {
            if (!included.contains(layer.id()) || layer.group() || layer.image.isNull())
                continue;
            subject.ids << layer.id();
            boxes << layer.transform();
            if (shown(*document_, layer))
                visible << layer.transform();
        }
        if (subject.ids.isEmpty())
            return {};
        subject.kind = TransformSubject::Kind::Group;
        subject.box = uprightBox(visible.isEmpty() ? boxes : visible);
        return subject;
    }
    if (active->group())
        return {};
    subject.kind = TransformSubject::Kind::Layer;
    subject.ids = {active->id()};
    subject.box = active->transform();
    return subject;
}
QVector<QPointF> Canvas::handlePoints(const QVector<QPointF> &c, bool rotation) const {
    const auto origin = canvasRect().topLeft();
    const QVector<QPointF> document{c[0],
                                    (c[0] + c[1]) / 2,
                                    c[1],
                                    (c[1] + c[2]) / 2,
                                    c[2],
                                    (c[2] + c[3]) / 2,
                                    c[3],
                                    (c[3] + c[0]) / 2};
    QVector<QPointF> points;
    for (const auto &p : document)
        points << origin + p * zoom;
    if (rotation) {
        const auto center = origin + (c[0] + c[2]) / 2 * zoom;
        const auto delta = points[1] - center;
        points << points[1] + delta * (24 / std::max(1.0, std::hypot(delta.x(), delta.y())));
    }
    return points;
}
void Canvas::beginTransform(const TransformSubject &subject, int handle, bool distort) {
    emit editStarted();
    transformSubject_ = subject;
    transformOriginals_.clear();
    for (const auto &id : subject.ids)
        if (const auto *layer = document_->find(id))
            transformOriginals_[id] = *layer;
    transformDraft_ = subject.box;
    transformHandle_ = handle;
    distorting_ = distort && handle < 8 && subject.kind != TransformSubject::Kind::Mask;
    if (distorting_)
        distortStart_ = distortCorners_ = transformCorners(subject.box);
    dragging_ = true;
    if (subject.kind == TransformSubject::Kind::Layer && !distorting_)
        beginLayerEdit(subject.ids.front());
}
QJsonObject Canvas::draftTransform(const QJsonObject &box, int handle, QPointF point,
                                   Qt::KeyboardModifiers mods, const QStringList &ids) const {
    const QPointF origin = pairOf(box, "origin"), size = pairOf(box, "size");
    const QRectF b(origin, QSizeF(size.x(), size.y()));
    const double angle = box.value("rotation").toDouble();
    QTransform rotation;
    rotation.translate(b.center().x(), b.center().y());
    rotation.rotate(angle);
    rotation.translate(-b.center().x(), -b.center().y());
    auto result = box;
    if (handle == 8) {
        const auto a = point - b.center(), from = start_ - b.center();
        double turned = (std::atan2(a.y(), a.x()) - std::atan2(from.y(), from.x())) * 180 /
                            std::numbers::pi +
                        angle;
        if (mods & Qt::ShiftModifier)
            turned = std::round(turned / 15) * 15;
        result["rotation"] = turned;
        return result;
    }
    point = rotation.inverted().map(point);
    QRectF next = b;
    const bool left = handle == 0 || handle == 6 || handle == 7;
    const bool right = handle == 2 || handle == 3 || handle == 4;
    const bool top = handle <= 2, bottom = handle >= 4 && handle <= 6;
    const bool centered = mods & Qt::AltModifier;
    if (std::abs(angle) < .001) {
        const QSet<QString> exclude(ids.begin(), ids.end());
        if (left || right)
            point.setX(snapValue(point.x(), false, mods, exclude));
        if (top || bottom)
            point.setY(snapValue(point.y(), true, mods, exclude));
    }
    if (left)
        next.setLeft(point.x());
    if (right)
        next.setRight(point.x());
    if (top)
        next.setTop(point.y());
    if (bottom)
        next.setBottom(point.y());
    if (centered) {
        const double halfWidth = (left || right) ? std::abs(point.x() - b.center().x()) : b.width() / 2;
        const double halfHeight = (top || bottom) ? std::abs(point.y() - b.center().y()) : b.height() / 2;
        next = QRectF(b.center() - QPointF(halfWidth, halfHeight),
                      QSizeF(halfWidth * 2, halfHeight * 2));
    }
    if (mods & Qt::ShiftModifier) {
        const double ratio = b.width() / b.height();
        double width = std::abs(next.width()), height = std::abs(next.height());
        if (!(top || bottom) || ((left || right) && std::abs(width / b.width() - 1) >
                                                        std::abs(height / b.height() - 1)))
            height = width / ratio;
        else
            width = height * ratio;
        const QPointF anchor(centered ? b.center().x()
                             : left   ? b.right()
                                      : b.left(),
                             centered ? b.center().y()
                             : top    ? b.bottom()
                                      : b.top());
        next = QRectF(anchor - QPointF(centered ? width / 2
                                       : left   ? width
                                                : 0,
                                       centered ? height / 2
                                       : top    ? height
                                                : 0),
                      QSizeF(width, height));
    }
    next = next.normalized();
    if (next.width() < 1 || next.height() < 1)
        return {};
    next.moveCenter(rotation.map(next.center()));
    result["origin"] = QJsonArray{next.x(), next.y()};
    result["size"] = QJsonArray{next.width(), next.height()};
    return result;
}
void Canvas::applyTransform(const QJsonObject &draft) {
    const auto &subject = transformSubject_;
    QHash<QString, QJsonObject> moved;
    for (const auto &id : subject.ids) {
        const auto &original = transformOriginals_.value(id);
        if (subject.kind != TransformSubject::Kind::Group)
            continue;
        const auto next = followedTransform(original.transform(), subject.box, draft);
        const auto size = pairOf(next, "size");
        // A member scaled to nothing would not be a valid layer; keep the last good shape.
        if (!std::isfinite(size.x()) || !std::isfinite(size.y()) || size.x() < 1 || size.y() < 1)
            return;
        moved[id] = next;
    }
    transformDraft_ = draft;
    for (const auto &id : subject.ids) {
        auto *layer = document_->find(id);
        if (!layer)
            continue;
        *layer = transformOriginals_.value(id);
        switch (subject.kind) {
        case TransformSubject::Kind::Mask:
            layer->metadata["maskPlacement"] = draft;
            break;
        case TransformSubject::Kind::Layer:
            retransformLayer(*layer, draft);
            break;
        case TransformSubject::Kind::Group:
            retransformLayer(*layer, moved.value(id));
            break;
        case TransformSubject::Kind::None:
            break;
        }
    }
    emit edited();
    refresh();
}
void Canvas::updateDistort(QPointF point, Qt::KeyboardModifiers mods) {
    auto next = distortStart_;
    double dx = point.x() - start_.x(), dy = point.y() - start_.y();
    // Shift keeps what is dragged on one axis.
    if (mods & Qt::ShiftModifier) {
        if (std::abs(dx) >= std::abs(dy))
            dy = 0;
        else
            dx = 0;
    }
    for (const int corner : distortedCorners(transformHandle_))
        next[corner] += QPointF(dx, dy);
    // A twisted or collapsed shape is ignored; the last usable one stays.
    if (distortUsable(next))
        distortCorners_ = next;
}
void Canvas::applyDistortion(double limit) {
    const auto &subject = transformSubject_;
    for (const auto &id : subject.ids) {
        auto *layer = document_->find(id);
        if (!layer)
            continue;
        Layer copy = transformOriginals_.value(id);
        const auto transform = copy.transform();
        const auto corners = subject.kind == TransformSubject::Kind::Group
                                 ? carriedCorners(transform, subject.box, distortCorners_)
                                 : distortCorners_;
        if (distortUsable(corners))
            distortLayer(copy, transform, corners, limit);
        *layer = copy;
    }
    emit edited();
    refresh();
}
void Canvas::endTransform() {
    transformHandle_ = -1;
    distorting_ = false;
    duplicating_ = false;
    transformSubject_ = {};
    transformOriginals_.clear();
    transformDraft_ = {};
    distortStart_.clear();
    distortCorners_.clear();
}
void Canvas::drawTransformControls(QPainter &p) {
    if (session_->tool != Tool::Move || !session_->view.transformControls)
        return;
    QVector<QPointF> corners;
    bool rotation = true;
    if (transformHandle_ >= 0 && distorting_) {
        corners = distortCorners_;
        rotation = false;
    } else if (transformHandle_ >= 0 && !transformDraft_.isEmpty()) {
        corners = transformCorners(transformDraft_);
    } else {
        const auto subject = transformSubject();
        if (subject.kind == TransformSubject::Kind::None)
            return;
        corners = transformCorners(subject.box);
    }
    const auto points = handlePoints(corners, rotation);
    p.setPen(QPen(QColor(75, 180, 255), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(QPolygonF{points[0], points[2], points[4], points[6]});
    if (rotation)
        p.drawLine(points[1], points[8]);
    p.setBrush(Qt::white);
    for (int i = 0; i < 8; ++i)
        p.drawRect(QRectF(points[i] - QPointF(3, 3), QSizeF(6, 6)));
    if (rotation)
        p.drawEllipse(points[8], 4, 4);
}

void Canvas::beginMove(bool duplicate) {
    duplicating_ = false;
    transformOriginals_.clear();
    transformSubject_ = {};
    QSet<QString> selected = session_->selectedLayerIDs;
    selected.insert(document_->activeId());
    const bool maskOnly = paintMask() && document_->active() && !document_->active()->group() &&
                          !document_->active()->mask.isNull();
    // Option-drag leaves the originals and drags copies of the selected roots and their contents.
    if (duplicate && !maskOnly) {
        const Document source = *document_;
        const auto copies = copyLayers(*document_, source, selected, std::nullopt, true);
        session_->selectedLayerIDs = QSet<QString>(copies.begin(), copies.end());
        selected = session_->selectedLayerIDs;
        duplicating_ = true;
    }
    QSet<QString> moving = maskOnly ? QSet<QString>{document_->activeId()}
                                    : layerSubtrees(*document_, selected);
    moveFrame_ = {};
    for (const auto &layer : document_->layers) {
        if (!moving.contains(layer.id()))
            continue;
        transformSubject_.ids << layer.id();
        transformOriginals_[layer.id()] = layer;
        if (layer.group() && !maskOnly)
            continue;
        const auto target = maskOnly ? transformTarget(layer) : layer;
        const auto image = maskOnly ? layer.mask : layer.image;
        const auto source = image.isNull() ? QSize(1, 1) : image.size();
        if (image.isNull() && !maskOnly && layer.metadata.value("adjustment").isObject())
            continue;
        moveFrame_ |= target.placement(source).mapRect(QRectF(QPointF(), source));
    }
    transformSubject_.kind =
        maskOnly ? TransformSubject::Kind::Mask : TransformSubject::Kind::Layer;
    if (transformSubject_.ids.size() == 1 && !duplicating_)
        beginLayerEdit(transformSubject_.ids.front());
}
void Canvas::moveBy(QPointF delta, Qt::KeyboardModifiers mods) {
    if (mods & Qt::ShiftModifier) {
        if (std::abs(delta.x()) > std::abs(delta.y()))
            delta.setY(0);
        else
            delta.setX(0);
    }
    if (!(mods & Qt::ControlModifier) && !moveFrame_.isEmpty()) {
        const QSet<QString> exclude(transformSubject_.ids.begin(), transformSubject_.ids.end());
        auto adjustment =
            snapBounds(*document_, session_->view, moveFrame_.translated(delta), zoom, exclude);
        if (mods & Qt::ShiftModifier) {
            if (delta.x() == 0)
                adjustment.setX(0);
            else
                adjustment.setY(0);
        }
        delta += adjustment;
    }
    for (const auto &id : transformSubject_.ids) {
        auto *layer = document_->find(id);
        if (!layer)
            continue;
        const auto &original = transformOriginals_.value(id);
        layer->metadata = original.metadata;
        if (transformSubject_.kind == TransformSubject::Kind::Mask) {
            auto target = transformTarget(original);
            target.move(delta);
            layer->metadata["maskPlacement"] = target.transform();
        } else
            layer->move(delta);
    }
    emit edited();
    refresh();
}
void Canvas::finishMove() {
    const bool several = session_->selectedLayerIDs.size() > 1;
    const QString label = duplicating_ ? (several ? "Duplicate Layers" : "Duplicate Layer")
                                       : (several ? "Move Layers" : "Move Layer");
    endTransform();
    emit editFinished(label);
}
} // namespace compositor
