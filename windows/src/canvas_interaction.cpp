// SPDX-License-Identifier: MIT
#include "canvas.h"
#include "canvas_layout.h"
#include "canvas_tools.h"
#include "distort.h"
#include "layer_operations.h"
#include "render.h"
#include "shortcuts.h"
#include <QEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace compositor {
bool Canvas::brushTool() const {
    auto t = session_->tool;
    return t == Tool::Brush || t == Tool::Erase || t == Tool::Clone || t == Tool::Heal ||
           t == Tool::Blur || t == Tool::Smudge || t == Tool::Liquify;
}
Layer Canvas::transformTarget(const Layer &layer) const {
    Layer target = layer;
    if (paintMask() && !layer.mask.isNull()) {
        target.image = layer.mask;
        if (layer.metadata.value("maskPlacement").isObject())
            target.metadata["transform"] = layer.metadata["maskPlacement"];
    }
    return target;
}
void Canvas::selectLayerAt(QPointF point, bool extend) {
    for (int i = document_->layers.size() - 1; i >= 0; --i) {
        const auto &candidate = document_->layers[i];
        if (candidate.group() || candidate.image.isNull() || candidate.opacity() == 0)
            continue;
        bool visible = candidate.visible();
        for (auto parent = document_->find(candidate.parent()); parent;
             parent = document_->find(parent->parent()))
            visible &= parent->visible();
        if (!visible)
            continue;
        auto local = candidate.placement(candidate.image.size()).inverted().map(point);
        QPoint pixel(int(std::floor(local.x())), int(std::floor(local.y())));
        if (!candidate.image.valid(pixel) || candidate.image.pixelColor(pixel).alpha() == 0)
            continue;
        const auto id = candidate.id();
        document_->metadata["activeLayerID"] = id;
        if (!extend)
            session_->selectedLayerIDs.clear();
        session_->selectedLayerIDs.insert(id);
        session_->target = EditTarget::Pixels;
        emit sessionChanged();
        update();
        return;
    }
}
QColor Canvas::sampleColor(QPointF position, int sampleSize) {
    const QPoint pixel(int(std::floor(position.x())), int(std::floor(position.y())));
    const auto size = document_->size();
    if (!QRect(QPoint(), size).contains(pixel))
        return Qt::transparent;
    const int radius = sampleSize / 2;
    const auto area = QRect(pixel - QPoint(radius, radius), QSize(sampleSize, sampleSize))
                          .intersected(QRect(QPoint(), size));
    auto image = renderArea(*document_, {size, area});
    double red = 0, green = 0, blue = 0, alpha = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto color = image.pixelColor(x, y);
            const auto a = color.alphaF();
            red += color.redF() * a;
            green += color.greenF() * a;
            blue += color.blueF() * a;
            alpha += a;
        }
    return alpha > 0 ? QColor::fromRgbF(red / alpha, green / alpha, blue / alpha,
                                        alpha / (image.width() * image.height()))
                     : QColor(Qt::transparent);
}
double Canvas::snapValue(double v, bool horizontal, Qt::KeyboardModifiers mods,
                         const QSet<QString> &exclude) const {
    return mods & Qt::ControlModifier
               ? v
               : snapCoordinate(*document_, session_->view, v, horizontal, zoom, exclude);
}
void Canvas::zoomTo(double value, QPointF anchor) {
    if (anchor.isNull())
        anchor = QRectF(rect()).center();
    auto point = toDocument(anchor);
    zoom = std::clamp(value, .01, 64.0);
    pan_ += anchor - (canvasRect().topLeft() + point * zoom);
    fitted_ = true;
    update();
    emit sessionChanged();
}
void Canvas::cycleToolMode() {
    const QVector<QVector<Tool>> groups{{Tool::Brush, Tool::Erase},
                                        {Tool::RectangleSelect, Tool::EllipseSelect},
                                        {Tool::Rectangle, Tool::Ellipse, Tool::Line},
                                        {Tool::Blur, Tool::Smudge, Tool::Liquify}};
    for (const auto &group : groups)
        if (group.contains(session_->tool)) {
            setTool(group[(group.indexOf(session_->tool) + 1) % group.size()]);
            emit sessionChanged();
            return;
        }
}
void Canvas::addGuide(bool horizontal, double position) {
    if (session_->view.lockGuides || !std::isfinite(position))
        return;
    cancelInteraction();
    emit editStarted();
    auto guides = document_->metadata.value("guides").toArray();
    guides.append(QJsonObject{
        {"id", newId()}, {"axis", horizontal ? "horizontal" : "vertical"}, {"position", position}});
    document_->metadata["guides"] = guides;
    emit editFinished("New Guide");
    refresh();
}
void Canvas::clearGuides() {
    if (session_->view.lockGuides || document_->metadata.value("guides").toArray().isEmpty())
        return;
    cancelInteraction();
    emit editStarted();
    document_->metadata["guides"] = QJsonArray();
    emit editFinished("Clear Guides");
    refresh();
}
bool Canvas::event(QEvent *event) {
    if (event->type() == QEvent::ShortcutOverride) {
        auto key = static_cast<QKeyEvent *>(event);
        const auto mapped = Shortcuts::instance().canvasKey(key->keyCombination());
        const auto entries = Shortcuts::instance().entries();
        for (const auto &entry : entries)
            if (entry.group == "Canvas" && !Shortcuts::instance().sequence(entry).isEmpty() &&
                Shortcuts::instance().sequence(entry)[0] == key->keyCombination()) {
                event->accept();
                return true;
            }
        if (mapped.key() == Qt::Key_unknown || mapped != key->keyCombination()) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent *>(event)->key() == Qt::Key_Tab) {
        keyPressEvent(static_cast<QKeyEvent *>(event));
        return true;
    }
    return QWidget::event(event);
}
void Canvas::nudge(QPointF delta, bool pixels) {
    if (dragging_)
        return;
    if (!pixels &&
        (session_->tool == Tool::RectangleSelect || session_->tool == Tool::EllipseSelect ||
         session_->tool == Tool::Lasso || session_->tool == Tool::Wand)) {
        if (session_->selection.isNull())
            return;
        QImage next(document_->size(), QImage::Format_RGBA8888_Premultiplied);
        next.fill(Qt::transparent);
        QPainter p(&next);
        p.drawImage(delta, session_->selection);
        p.end();
        replaceSelection(next.convertToFormat(QImage::Format_Grayscale8), "Move Selection");
        return;
    }
    auto l = document_->active();
    if (!l)
        return;
    if (pixels) {
        if (session_->selection.isNull() || l->group())
            return;
        auto &target = paintMask() ? l->mask : l->image;
        if (target.isNull())
            return;
        const auto coverage = selectionForLayer(transformTarget(*l));
        auto base = target.convertToFormat(QImage::Format_RGBA8888_Premultiplied), payload = base;
        payload.detach();
        base.detach();
        for (int y = 0; y < base.height(); ++y)
            for (int x = 0; x < base.width(); ++x) {
                int amount = coverage.constScanLine(y)[x];
                auto a = base.scanLine(y) + x * 4, b = payload.scanLine(y) + x * 4;
                for (int c = 0; c < 4; ++c) {
                    b[c] = uchar((int(b[c]) * amount + 127) / 255);
                    a[c] = uchar((int(a[c]) * (255 - amount) + 127) / 255);
                }
            }
        auto inverse = l->placement(target.size()).inverted();
        if (paintMask() && l->metadata.value("maskPlacement").isObject()) {
            auto mask = transformTarget(*l);
            inverse = mask.placement(target.size()).inverted();
        }
        QPainter p(&base);
        p.drawImage(inverse.map(delta) - inverse.map(QPointF()), payload);
        p.end();
        emit editStarted();
        l = document_->active();
        (paintMask() ? l->mask : l->image) =
            paintMask() ? base.convertToFormat(QImage::Format_Grayscale8) : base;
        if (!paintMask()) {
            l->metadata.remove("text");
            l->metadata.remove("shape");
        }
        QImage selection(document_->size(), QImage::Format_RGBA8888_Premultiplied);
        selection.fill(Qt::transparent);
        QPainter s(&selection);
        s.drawImage(delta, session_->selection);
        s.end();
        session_->selection = selection.convertToFormat(QImage::Format_Grayscale8);
        emit editFinished("Move Selected Pixels");
        emit selectionChanged();
        refresh();
        return;
    }
    if (session_->tool != Tool::Move)
        return;
    emit editStarted();
    l = document_->active();
    if (paintMask() && !l->mask.isNull()) {
        auto m = l->metadata.value("maskPlacement").toObject();
        if (m.isEmpty())
            m = l->transform();
        auto a = m.value("origin").toArray();
        m["origin"] = QJsonArray{a[0].toDouble() + delta.x(), a[1].toDouble() + delta.y()};
        l->metadata["maskPlacement"] = m;
    } else {
        auto ids = layerSubtrees(*document_, session_->selectedLayerIDs.isEmpty()
                                                 ? QSet<QString>{l->id()}
                                                 : session_->selectedLayerIDs);
        for (auto &layer : document_->layers)
            if (ids.contains(layer.id()))
                layer.move(delta);
    }
    emit editFinished("Nudge Layer");
    refresh();
}
bool Canvas::handleCanvasKey(QKeyEvent *input) {
    auto combination = Shortcuts::instance().canvasKey(input->keyCombination());
    if (combination.key() == Qt::Key_unknown)
        return true;
    QKeyEvent event(QEvent::KeyPress, combination.key(), combination.keyboardModifiers(),
                    input->text(), input->isAutoRepeat(), input->count());
    auto e = &event;
    const auto mods = e->modifiers();
    const int key = e->key();
    if (controller().keyPress(e))
        return true;
    if (key == Qt::Key_Escape) {
        controller().cancel();
        guideDrag_ = -1;
        transformHandle_ = -1;
        return true;
    }
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && mods == Qt::NoModifier) {
        if (dragging_) {
            QMouseEvent release(QEvent::MouseButtonRelease, hover_, mapToGlobal(hover_),
                                Qt::LeftButton, Qt::NoButton, mods);
            mouseReleaseEvent(&release);
        }
        return true;
    }
    if (key == Qt::Key_Space && mods == Qt::NoModifier) {
        if (!e->isAutoRepeat() && !dragging_) {
            temporaryPan_ = true;
            panPhysicalKey_ = input->key();
            setCursor(Qt::OpenHandCursor);
        }
        return true;
    }
    if (dragging_)
        return false;
    if (key == Qt::Key_Tab && mods == Qt::NoModifier) {
        cycleToolMode();
        return true;
    }
    if (key == Qt::Key_U && mods == Qt::ShiftModifier) {
        if (session_->tool == Tool::Rectangle || session_->tool == Tool::Ellipse ||
            session_->tool == Tool::Line)
            cycleToolMode();
        else {
            setTool(Tool::Rectangle);
            emit sessionChanged();
        }
        return true;
    }
    if (key == Qt::Key_M && mods == Qt::ShiftModifier) {
        if (session_->tool == Tool::RectangleSelect || session_->tool == Tool::EllipseSelect)
            cycleToolMode();
        else {
            setTool(Tool::RectangleSelect);
            emit sessionChanged();
        }
        return true;
    }
    if (key == Qt::Key_R && mods == Qt::ShiftModifier) {
        if (session_->tool == Tool::Blur || session_->tool == Tool::Smudge ||
            session_->tool == Tool::Liquify)
            cycleToolMode();
        else {
            setTool(Tool::Blur);
            emit sessionChanged();
        }
        return true;
    }
    if ((key == Qt::Key_BracketLeft || key == Qt::Key_BracketRight) &&
        (mods == Qt::NoModifier || mods == Qt::ShiftModifier) && brushTool()) {
        double direction = key == Qt::Key_BracketRight ? 1 : -1;
        if (mods == Qt::ShiftModifier)
            session_->hardness = std::clamp(session_->hardness + direction * .1, 0.0, 1.0);
        else {
            double step = session_->brushSize < 10    ? 1
                          : session_->brushSize < 50  ? 5
                          : session_->brushSize < 200 ? 10
                                                      : 50;
            session_->brushSize = std::clamp(session_->brushSize + direction * step, 1.0, 2000.0);
        }
        emit sessionChanged();
        update();
        return true;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9 && mods == Qt::NoModifier) {
        int digit = key - Qt::Key_0, percent = digit == 0 ? 100 : digit * 10;
        if (opacityDigit_ >= 0 && opacityClock_.isValid() && opacityClock_.elapsed() < 650) {
            percent = opacityDigit_ * 10 + digit;
            opacityDigit_ = -1;
        } else {
            opacityDigit_ = digit;
            opacityClock_.restart();
        }
        if (brushTool() || session_->tool == Tool::Gradient)
            session_->brushOpacity = percent / 100.0;
        else if (document_->active()) {
            emit editStarted();
            document_->active()->metadata["opacity"] = percent / 100.0;
            emit editFinished("Layer Opacity");
            refresh();
        }
        emit sessionChanged();
        return true;
    }
    if ((key == Qt::Key_Minus || key == Qt::Key_Equal || key == Qt::Key_Plus) &&
        mods == Qt::ShiftModifier) {
        if (auto l = document_->active()) {
            auto modes = blendModes();
            int i = std::max(0, int(modes.indexOf(l->blend())));
            i = (i + (key == Qt::Key_Minus ? -1 : 1) + modes.size()) % modes.size();
            emit editStarted();
            document_->active()->metadata["blendMode"] = modes[i];
            emit editFinished("Blend Mode");
            refresh();
        }
        return true;
    }
    if (key == Qt::Key_X && mods == Qt::NoModifier) {
        std::swap(session_->foreground, session_->background);
        emit sessionChanged();
        return true;
    }
    if (key == Qt::Key_D && mods == Qt::NoModifier) {
        session_->foreground = Qt::black;
        session_->background = Qt::white;
        emit sessionChanged();
        return true;
    }
    if (key >= Qt::Key_Left && key <= Qt::Key_Down &&
        !(mods & (Qt::AltModifier | Qt::MetaModifier))) {
        double step = mods & Qt::ShiftModifier ? 10 : 1;
        nudge({key == Qt::Key_Left    ? -step
               : key == Qt::Key_Right ? step
                                      : 0,
               key == Qt::Key_Up     ? -step
               : key == Qt::Key_Down ? step
                                     : 0},
              mods & Qt::ControlModifier);
        return true;
    }
    return false;
}
bool Canvas::beginOverlayEdit(QMouseEvent *e) {
    if (temporaryPan_ || temporaryPicker_)
        return false;
    auto guides = document_->metadata.value("guides").toArray();
    bool fromHorizontal =
        session_->view.rulers && e->position().y() < 24 && e->position().x() >= 24;
    bool fromVertical = session_->view.rulers && e->position().x() < 24 && e->position().y() >= 24;
    if (!session_->view.lockGuides && session_->view.guides) {
        int index = -1;
        if (fromHorizontal || fromVertical) {
            index = guides.size();
            guides.append(QJsonObject{{"id", newId()},
                                      {"axis", fromHorizontal ? "horizontal" : "vertical"},
                                      {"position", fromHorizontal ? start_.y() : start_.x()}});
        } else if (session_->tool == Tool::Move || e->modifiers() & Qt::ControlModifier)
            for (int i = guides.size() - 1; i >= 0; --i) {
                auto g = guides[i].toObject();
                bool horizontal = g["axis"] == "horizontal";
                if (std::abs((horizontal ? start_.y() : start_.x()) - g["position"].toDouble()) *
                        zoom <
                    5) {
                    index = i;
                    break;
                }
            }
        if (index >= 0) {
            emit editStarted();
            guideDrag_ = index;
            document_->metadata["guides"] = guides;
            dragging_ = true;
            update();
            return true;
        }
    }
    if (session_->view.rulers && (e->position().x() < 24 || e->position().y() < 24))
        return true;
    if (session_->tool == Tool::Move && session_->view.transformControls) {
        const auto subject = transformSubject();
        if (subject.kind != TransformSubject::Kind::None) {
            const auto points = handlePoints(transformCorners(subject.box), true);
            const auto size = subject.box.value("size").toArray();
            const double hitRadius =
                std::min(7.0, std::max(1.5, std::min(size.at(0).toDouble(), size.at(1).toDouble()) *
                                                zoom * .2));
            for (int i = 0; i < points.size(); ++i)
                if (QLineF(points[i], e->position()).length() <= (i == 8 ? 7 : hitRadius)) {
                    // Ctrl-dragging a handle distorts: the box's corners move freely.
                    beginTransform(subject, i, e->modifiers() & Qt::ControlModifier);
                    return true;
                }
        }
    }
    return false;
}
bool Canvas::moveOverlayEdit(QMouseEvent *e) {
    if (guideDrag_ >= 0) {
        auto guides = document_->metadata["guides"].toArray();
        if (guideDrag_ >= guides.size())
            return true;
        auto g = guides[guideDrag_].toObject();
        bool horizontal = g["axis"] == "horizontal";
        // A moving guide does not snap to its previous position.
        Document copy = *document_;
        auto without = guides;
        without.removeAt(guideDrag_);
        copy.metadata["guides"] = without;
        auto point = toDocument(e->position());
        double v = horizontal ? point.y() : point.x();
        g["position"] = (e->modifiers() & Qt::ControlModifier)
                            ? v
                            : snapCoordinate(copy, session_->view, v, horizontal, zoom);
        guides[guideDrag_] = g;
        document_->metadata["guides"] = guides;
        update();
        return true;
    }
    if (transformHandle_ < 0)
        return false;
    const auto point = toDocument(e->position());
    if (distorting_) {
        updateDistort(point, e->modifiers());
        applyDistortion(1280);
        return true;
    }
    const auto draft = draftTransform(transformSubject_.box, transformHandle_, point,
                                      e->modifiers(), transformSubject_.ids);
    if (!draft.isEmpty())
        applyTransform(draft);
    return true;
}
bool Canvas::finishOverlayEdit(QMouseEvent *e) {
    if (guideDrag_ >= 0) {
        moveOverlayEdit(e);
        auto guides = document_->metadata["guides"].toArray();
        if (guideDrag_ < guides.size()) {
            auto g = guides[guideDrag_].toObject();
            bool horizontal = g["axis"] == "horizontal";
            auto v = g["position"].toDouble();
            if (v < 0 ||
                v > (horizontal ? document_->size().height() : document_->size().width()) ||
                (session_->view.rulers && (e->position().x() < 24 || e->position().y() < 24)))
                guides.removeAt(guideDrag_);
            document_->metadata["guides"] = guides;
        }
        guideDrag_ = -1;
        emit editFinished("Edit Guide");
        return true;
    }
    if (transformHandle_ >= 0) {
        QString label = transformSubject_.kind == TransformSubject::Kind::Group
                            ? "Transform Layers"
                            : "Transform Layer";
        if (distorting_) {
            updateDistort(toDocument(e->position()), e->modifiers());
            // A handle let go where it was grabbed distorts nothing.
            if (distortCorners_ == distortStart_) {
                endTransform();
                emit editCanceled();
                return true;
            }
            applyDistortion(0);
            label = transformSubject_.kind == TransformSubject::Kind::Group ? "Distort Layers"
                                                                           : "Distort";
        } else
            moveOverlayEdit(e);
        endTransform();
        emit editFinished(label);
        return true;
    }
    return false;
}
void Canvas::leaveEvent(QEvent *event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}
void Canvas::drawCanvasOverlay(QPainter &p) {
    const auto r = canvasRect();
    const auto &o = session_->view;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (o.grid) {
        p.save();
        p.setClipRect(r);
        double step = double(o.gridSpacing) / o.gridSubdivisions;
        if (step * zoom < 4)
            step = o.gridSpacing;
        if (step * zoom >= 4) {
            auto shown = r.intersected(rect());
            p.setPen(QPen(o.gridColor, 1));
            for (double x = std::ceil((shown.left() - r.left()) / zoom / step) * step;
                 x <= (shown.right() - r.left()) / zoom; x += step)
                p.drawLine(QPointF(r.left() + x * zoom, shown.top()),
                           QPointF(r.left() + x * zoom, shown.bottom()));
            for (double y = std::ceil((shown.top() - r.top()) / zoom / step) * step;
                 y <= (shown.bottom() - r.top()) / zoom; y += step)
                p.drawLine(QPointF(shown.left(), r.top() + y * zoom),
                           QPointF(shown.right(), r.top() + y * zoom));
        }
        p.restore();
    }
    drawTransformControls(p);
    if (hovered_ && !temporaryPan_) {
        if (brushTool() && !temporaryPicker_) {
            auto radius = session_->brushSize * zoom / 2;
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(Qt::black, 3));
            p.drawEllipse(hover_, radius, radius);
            p.setPen(QPen(Qt::white, 1));
            p.drawEllipse(hover_, radius, radius);
            if (session_->hardness < 1) {
                p.setPen(QPen(Qt::white, 1, Qt::DashLine));
                p.drawEllipse(hover_, radius * session_->hardness, radius * session_->hardness);
            }
            if (session_->tool == Tool::Clone && cloneReady_) {
                auto source = (dragging_ || (session_->cloneAligned && cloneStrokeReady_))
                                  ? toDocument(hover_) + cloneOffset_
                                  : cloneSource_;
                auto at = r.topLeft() + source * zoom;
                p.setPen(QPen(Qt::cyan, 1));
                p.drawLine(at - QPointF(8, 0), at + QPointF(8, 0));
                p.drawLine(at - QPointF(0, 8), at + QPointF(0, 8));
            }
        }
        if (session_->tool == Tool::Eyedropper || temporaryPicker_) {
            p.setPen(QPen(Qt::white, 3));
            p.setBrush(hoverColor_);
            p.drawEllipse(hover_ + QPointF(28, -28), 19, 19);
            p.setPen(QPen(session_->foreground, 5));
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(hover_ + QPointF(9, -47), QSizeF(38, 38)), 180 * 16, 180 * 16);
            p.setPen(QPen(Qt::white, 1));
            p.drawLine(hover_ - QPointF(5, 0), hover_ + QPointF(5, 0));
            p.drawLine(hover_ - QPointF(0, 5), hover_ + QPointF(0, 5));
        }
    }
    if (o.rulers) {
        p.fillRect(QRect(0, 0, width(), 24), QColor(48, 50, 56));
        p.fillRect(QRect(0, 0, 24, height()), QColor(48, 50, 56));
        p.setPen(QColor(210, 215, 225));
        auto font = p.font();
        font.setPixelSize(10);
        p.setFont(font);
        double step = std::pow(10, std::floor(std::log10(60 / std::max(.001, zoom))));
        if (step * zoom < 30)
            step *= 5;
        else if (step * zoom < 50)
            step *= 2;
        for (int axis = 0; axis < 2; ++axis) {
            double origin = axis ? r.top() : r.left(), end = axis ? height() : width();
            for (double value = std::ceil((24 - origin) / zoom / step) * step;
                 origin + value * zoom < end; value += step) {
                double at = origin + value * zoom;
                if (axis) {
                    p.drawLine(QPointF(17, at), QPointF(24, at));
                    p.save();
                    p.translate(4, at + 3);
                    p.rotate(-90);
                    p.drawText(QPointF(), QString::number(value));
                    p.restore();
                } else {
                    p.drawLine(QPointF(at, 17), QPointF(at, 24));
                    p.drawText(QPointF(at + 3, 12), QString::number(value));
                }
                for (int i = 1; i < 5; ++i) {
                    double minor = at + i * step * zoom / 5;
                    if (axis)
                        p.drawLine(QPointF(21, minor), QPointF(24, minor));
                    else
                        p.drawLine(QPointF(minor, 21), QPointF(minor, 24));
                }
            }
        }
        p.fillRect(QRect(0, 0, 24, 24), QColor(60, 63, 70));
        if (hovered_) {
            p.setPen(Qt::cyan);
            p.drawLine(QPointF(hover_.x(), 0), QPointF(hover_.x(), 24));
            p.drawLine(QPointF(0, hover_.y()), QPointF(24, hover_.y()));
        }
    }
    p.restore();
}
} // namespace compositor
