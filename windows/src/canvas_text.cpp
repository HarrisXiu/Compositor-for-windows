// SPDX-License-Identifier: MIT
#include "canvas.h"
#include "canvas_text_editor.h"
#include "distort.h"
#include "editable_layers.h"
#include <QGuiApplication>
#include <QHideEvent>
#include <QInputMethod>
#include <QJsonArray>
#include <QTextEdit>
#include <algorithm>

namespace compositor {
namespace {
// A new text raster retains the old source grid's top-left in document coordinates.
void replaceTextImage(Layer &layer, QImage image) {
    if (image.size() != layer.image.size()) {
        const auto mapping = layer.placement(layer.image.size());
        auto transform = layer.transform();
        const auto oldSize = transform["size"].toArray();
        const QSizeF size(oldSize[0].toDouble() * image.width() / layer.image.width(),
                          oldSize[1].toDouble() * image.height() / layer.image.height());
        const auto center = mapping.map(QPointF(image.width() / 2.0, image.height() / 2.0));
        if (!layer.mask.isNull() && !layer.metadata.value("maskPlacement").isObject())
            layer.metadata["maskPlacement"] = transform;
        transform["origin"] =
            QJsonArray{center.x() - size.width() / 2, center.y() - size.height() / 2};
        transform["size"] = QJsonArray{size.width(), size.height()};
        layer.metadata["transform"] = transform;
    }
    layer.image = std::move(image);
}
} // namespace
QString Canvas::textLayerAt(QPointF point) const {
    for (int i = document_->layers.size() - 1; i >= 0; --i) {
        const auto &layer = document_->layers[i];
        if (!layer.visible() || !layer.metadata.value("text").isObject())
            continue;
        bool visible = true;
        for (auto parent = document_->find(layer.parent()); parent;
             parent = document_->find(parent->parent()))
            visible &= parent->visible();
        if (visible && QRectF(QPointF(), layer.image.size())
                           .contains(layer.placement(layer.image.size()).inverted().map(point)))
            return layer.id();
    }
    return {};
}
void Canvas::beginTextEditing(const QString &id, QRectF bounds) {
    if (textEditor_ && id == textLayerID_) {
        textEditor_->text()->setFocus();
        return;
    }
    finishTextEditing(true);
    cancelInteraction();
    const auto *source = document_->find(id);
    if (!id.isEmpty())
        require(source && source->metadata.value("text").isObject(),
                "Select an editable text layer");
    textNew_ = id.isEmpty();
    textBefore_ = source ? *source : Layer();
    emit editStarted();
    try {
        if (textNew_) {
            if (bounds.width() < 16 || bounds.height() < 16)
                bounds.setSize(QSizeF(320, std::max(128.0, session_->textSize * 1.5 + 24)));
            QJsonObject style{{"content", ""},
                              {"fontName", session_->textFont},
                              {"fontSize", session_->textSize},
                              {"tracking", session_->textTracking},
                              {"leading", session_->textLeading},
                              {"alignment", session_->textAlignment},
                              {"red", session_->foreground.redF()},
                              {"green", session_->foreground.greenF()},
                              {"blue", session_->foreground.blueF()},
                              {"boxSize", QJsonArray{bounds.width(), bounds.height()}}};
            textLayerID_ = document_->addImage("Text", renderText(style));
            auto layer = document_->find(textLayerID_);
            layer->setBounds(bounds);
            layer->metadata["text"] = style;
        } else
            textLayerID_ = id;
        document_->metadata["activeLayerID"] = textLayerID_;
        session_->selectedLayerIDs = {textLayerID_};
        session_->target = EditTarget::Pixels;
        session_->tool = Tool::Text;
        textEditor_ =
            new CanvasTextEditor(document_->find(textLayerID_)->metadata["text"].toObject(), this);
        textEditor_->changed = [this] { updateTextEditing(); };
        textEditor_->fontsChanged = [this] { updateTextEditing(true); };
        textEditor_->apply = [this] { finishTextEditing(true); };
        textEditor_->cancel = [this] { finishTextEditing(false); };
        textEditor_->frameStarted = [this] { textFrameBefore_ = *document_->find(textLayerID_); };
        textEditor_->frameChanged = [this](QRectF rect) {
            try {
                auto layer = document_->find(textLayerID_);
                *layer = textFrameBefore_;
                const auto mapping = layer->placement(layer->image.size());
                if (!textEditor_->resizingFrame()) {
                    layer->move(mapping.map(rect.topLeft()) - mapping.map(QPointF()));
                } else {
                    textEditor_->setBoxSize(rect.size());
                    auto transform = layer->transform();
                    const auto oldSize = transform["size"].toArray();
                    QSizeF size(rect.width() * oldSize[0].toDouble() / layer->image.width(),
                                rect.height() * oldSize[1].toDouble() / layer->image.height());
                    const auto center = mapping.map(rect.center());
                    transform["origin"] =
                        QJsonArray{center.x() - size.width() / 2, center.y() - size.height() / 2};
                    transform["size"] = QJsonArray{size.width(), size.height()};
                    retransformLayer(*layer, transform);
                    auto style = textEditor_->style();
                    layer->image = renderText(style);
                    layer->metadata["text"] = style;
                }
                emit edited();
                refresh();
            } catch (const std::exception &error) {
                finishTextEditing(false);
                emit this->error(QString::fromUtf8(error.what()));
            }
        };
        forgetTiles();
        refresh();
        syncTextEditor();
        emit sessionChanged();
    } catch (const std::exception &) {
        if (textEditor_) {
            textEditor_->hideEditor();
            textEditor_->deleteLater();
            textEditor_ = nullptr;
        }
        textLayerID_.clear();
        emit editCanceled();
        throw;
    }
}
void Canvas::updateTextEditing(bool force) {
    if (!textEditor_)
        return;
    try {
        auto layer = document_->find(textLayerID_);
        require(layer, "Select an editable text layer");
        auto style = textEditor_->style();
        if (!force && style == layer->metadata["text"].toObject())
            return;
        auto image = renderText(style);
        replaceTextImage(*layer, std::move(image));
        layer->metadata["text"] = style;
        emit edited();
        refresh();
    } catch (const std::exception &error) {
        finishTextEditing(false);
        emit this->error(QString::fromUtf8(error.what()));
    }
}
void Canvas::finishTextEditing(bool apply) {
    if (!textEditor_)
        return;
    if (apply)
        QGuiApplication::inputMethod()->commit();
    else
        QGuiApplication::inputMethod()->reset();
    if (!textEditor_)
        return;
    const auto layer = document_->find(textLayerID_);
    const bool changed =
        layer &&
        (textNew_ ? !layer->metadata["text"].toObject()["content"].toString().isEmpty()
                  : layer->metadata != textBefore_.metadata || layer->image != textBefore_.image);
    auto editor = textEditor_;
    textEditor_ = nullptr;
    textLayerID_.clear();
    textBefore_ = {};
    textFrameBefore_ = {};
    editor->changed = {};
    editor->fontsChanged = {};
    editor->apply = {};
    editor->cancel = {};
    editor->frameStarted = {};
    editor->frameChanged = {};
    editor->hideEditor();
    editor->deleteLater();
    forgetTiles();
    if (apply && changed)
        emit editFinished(textNew_ ? "Text Layer" : "Edit Text");
    else
        emit editCanceled();
    refresh();
    setFocus();
}
void Canvas::syncTextEditor() {
    if (!textEditor_)
        return;
    const auto layer = document_->find(textLayerID_);
    if (!layer)
        return;
    QTransform view;
    view.translate(canvasRect().left(), canvasRect().top());
    view.scale(zoom, zoom);
    textEditor_->place(layer->placement(layer->image.size()) * view, layer->image.size());
}
void Canvas::hideEvent(QHideEvent *event) {
    finishTextEditing(true);
    QWidget::hideEvent(event);
}
} // namespace compositor
