// SPDX-License-Identifier: MIT
#include "canvas.h"
#include "editable_layers.h"
#include "effects.h"
#include "filters.h"
#include "render.h"
#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTextDocument>
#include <QUrl>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
extern "C" {
#include "HealPixels.h"
#include "WandPixels.h"
}

namespace compositor {
void Canvas::beginPaint(QMouseEvent *) {
    auto layer = document_->active();
    require(layer, "Select a layer first");
    require(!layer->group() || paintMask(), "Select an image layer or a folder mask");

    require(!(paintMask() ? layer->mask : layer->image).isNull(), "Select a pixel layer or mask");
    require(!paintMask() || session_->tool == Tool::Brush || session_->tool == Tool::Erase ||
                session_->tool == Tool::Blur,
            "Mask painting supports Brush, Eraser, and Blur");
    if (session_->tool == Tool::Clone)
        require(cloneReady_, "Alt-click the canvas to set a clone source");
    require(!paintMask() || !layer->mask.isNull(), "Add a mask before painting it");
    emit editStarted();
    beginLayerEdit(layer->id());
    strokeArea_ = {};
    strokePixels_ = {};
    dragging_ = true;
    original_ = paintMask() ? layer->mask : layer->image;
    carriedKey_ = original_.cacheKey();
    strokeReach_ = 0;
    if (auto effects = layer->metadata.value("effects").toObject();
        !effects.isEmpty() && !layer->image.isNull()) {
        const auto placement = layer->placement(layer->image.size());
        strokeReach_ =
            effectsReach(effects) * std::max(std::hypot(placement.m11(), placement.m12()),
                                             std::hypot(placement.m21(), placement.m22()));
    }
    if (session_->tool == Tool::Smudge || session_->tool == Tool::Liquify) {
        warp_ = std::make_unique<WarpBrush>(
            original_, layer->placement(original_.size()), document_->size(),
            session_->tool == Tool::Smudge ? WarpMode::Smudge : WarpMode::Liquify,
            session_->brushSize, session_->hardness, session_->brushOpacity);
        warp_->append(start_);
        warpChanged_ = false;
        return;
    }
    if (session_->tool == Tool::Blur) {
        auto placement = layer->placement(original_.size());
        if (paintMask() && layer->metadata.value("maskPlacement").isObject()) {
            Layer maskLayer = *layer;
            maskLayer.metadata["transform"] = layer->metadata.value("maskPlacement");
            placement = maskLayer.placement(original_.size());
        }
        double scale = std::max(.000001, std::sqrt(std::abs(placement.determinant())));
        blurred_ =
            gaussianBlur(original_,
                         std::min({std::clamp(session_->blurRadius, .5, 50.0) / scale,
                                   std::max(original_.width(), original_.height()) / 2.0, 250.0}),
                         paintMask());
        if (paintMask())
            blurred_ = blurred_.convertToFormat(QImage::Format_Grayscale8);
    }
    coverage_ = QImage(original_.size(), QImage::Format_Grayscale8);
    require(!coverage_.isNull(), "Not enough memory for stroke");
    coverage_.fill(0);
    cloneOffset_ = cloneSource_ - start_;
    if (session_->tool != Tool::Gradient)
        dab(start_);
    refreshStroke();
}
void Canvas::continuePaint(QMouseEvent *e) {
    auto point = toDocument(e->position());
    if (warp_) {
        if (warp_->append(point)) {
            warpChanged_ = true;
            auto layer = document_->active();
            layer->image = warp_->result(original_, layer->placement(original_.size()),
                                         selectionForLayer(*layer));
            layer->metadata.remove("text");
            layer->metadata.remove("shape");
            emit edited();
            refreshArea(layerExtent(*layer));
        }
    } else if (session_->tool == Tool::Brush || session_->tool == Tool::Erase ||
               session_->tool == Tool::Clone || session_->tool == Tool::Heal ||
               session_->tool == Tool::Blur) {
        auto delta = point - last_;
        int steps = std::max(1, int(std::ceil(std::hypot(delta.x(), delta.y()) /
                                              std::max(1.0, session_->brushSize * 0.1))));
        steps = std::min(steps, 1000);
        for (int i = 1; i <= steps; ++i)
            dab(last_ + delta * (double(i) / steps));
        refreshStroke();
    } else
        update();

    last_ = point;
}
void Canvas::finishPaint(QMouseEvent *) {
    auto layer = document_->active();
    if (!layer)
        return;
    if (warp_) {
        if (warp_->append(last_))
            warpChanged_ = true;
        if (warpChanged_) {
            layer->image = warp_->result(original_, layer->placement(original_.size()),
                                         selectionForLayer(*layer));
            layer->metadata.remove("text");
            layer->metadata.remove("shape");
        }
        warp_.reset();
        if (warpChanged_)
            emit editFinished(session_->tool == Tool::Smudge ? "Smudge" : "Liquify");
        else
            emit editCanceled();
        original_ = {};
        refresh();
        return;
    }
    if (session_->tool == Tool::Gradient) {
        auto from = layer->placement(layer->image.size()).inverted().map(start_),
             to = layer->placement(layer->image.size()).inverted().map(last_);
        QImage painted = original_;
        painted.detach();
        QPainter p(&painted);
        QLinearGradient gradient(from, to);
        gradient.setColorAt(0, session_->foreground);
        gradient.setColorAt(1, Qt::transparent);
        p.fillRect(painted.rect(), gradient);
        p.end();
        layer->image = limitToSelection(original_, painted, selectionForLayer(*layer));
        layer->metadata.remove("text");
        layer->metadata.remove("shape");
    }
    if (session_->tool == Tool::Heal) {
        require(!paintMask(), "Spot Healing currently targets image pixels");
        auto image = original_;
        image.detach();
        std::vector<uchar> packed(size_t(coverage_.width()) * coverage_.height());
        for (int y = 0; y < coverage_.height(); ++y)
            std::copy_n(coverage_.constScanLine(y), coverage_.width(),
                        packed.data() + size_t(y) * coverage_.width());
        require(spot_heal(image.bits(), packed.data(), size_t(image.width()),
                          size_t(image.height()), size_t(image.bytesPerLine()),
                          float(session_->brushOpacity), 0, 1) == 0,
                "Spot healing ran out of memory");
        layer->image = image;
        layer->metadata.remove("text");
        layer->metadata.remove("shape");
    }
    emit editFinished(session_->tool == Tool::Move       ? "Move Layer"
                      : session_->tool == Tool::Gradient ? "Gradient"
                      : session_->tool == Tool::Heal     ? "Spot Healing"
                      : session_->tool == Tool::Blur     ? "Blur Stroke"
                      : paintMask()                      ? "Paint Mask"
                                                         : "Brush Stroke");
    refresh();

    original_ = {};
    coverage_ = {};
    blurred_ = {};
    warp_.reset();
}
} // namespace compositor
