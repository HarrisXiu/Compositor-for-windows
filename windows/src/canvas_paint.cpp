// SPDX-License-Identifier: MIT
#include "canvas.h"
#include "editable_layers.h"
#include "effects.h"
#include "filters.h"
#include "paint_surface.h"
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
void Canvas::beginPaint(QMouseEvent *event) {
    auto layer = document_->active();
    require(layer, "Select a layer first");
    require(!layer->group() || paintMask(), "Select an image layer or a folder mask");

    require(!(paintMask() ? layer->mask : layer->image).isNull(), "Select a pixel layer or mask");
    require(!paintMask() || session_->tool == Tool::Brush || session_->tool == Tool::Erase ||
                session_->tool == Tool::Blur || session_->tool == Tool::Gradient,
            "Mask painting supports Brush, Eraser, Blur, and Gradient");
    if (session_->tool == Tool::Clone)
        require(cloneReady_, "Alt-click the canvas to set a clone source");
    require(!paintMask() || !layer->mask.isNull(), "Add a mask before painting it");
    emit editStarted();
    layer = document_->active();
    paintBefore_ = *layer;
    paintChanged_ = false;
    strokeSelectionBounds_ = session_->selection.isNull() ? QRect() : selectionBounds();
    cloneOffsetBefore_ = cloneOffset_;
    cloneReadyBefore_ = cloneStrokeReady_;
    if (paintMask())
        preparePaintMask(*layer);
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
        const double sigma = std::min({std::clamp(session_->blurRadius, .5, 50.0) / scale,
                                       std::max(original_.width(), original_.height()) / 2.0, 250.0});
        if (paintMask()) {
            // Past its pixels a mask keeps its edge tone, so blurring near its edge doesn't pull
            // in the wrong one (as on the Mac).
            const int margin = int(std::ceil(3 * sigma)) + 1;
            QImage padded(original_.width() + 2 * margin, original_.height() + 2 * margin,
                          QImage::Format_Grayscale8);
            require(!padded.isNull(), "Not enough memory for stroke");
            padded.fill(maskBackground(original_));
            const auto gray = original_.convertToFormat(QImage::Format_Grayscale8);
            for (int y = 0; y < gray.height(); ++y)
                std::copy_n(gray.constScanLine(y), gray.width(), padded.scanLine(y + margin) + margin);
            blurred_ = gaussianBlur(padded, sigma, true)
                           .copy(margin, margin, original_.width(), original_.height())
                           .convertToFormat(QImage::Format_Grayscale8);
        } else
            blurred_ = gaussianBlur(original_, sigma, false);
    }
    coverage_ = QImage(original_.size(), QImage::Format_Grayscale8);
    require(!coverage_.isNull(), "Not enough memory for stroke");
    coverage_.fill(0);
    if (!session_->cloneAligned || !cloneStrokeReady_)
        cloneOffset_ = cloneSource_ - start_;
    if (session_->tool == Tool::Clone) {
        cloneStrokeReady_ = true;
        cloneSample_ = session_->cloneMerged ? fullComposite() : original_;
        cloneSamplePlacement_ = session_->cloneMerged ? QTransform()
                                                     : layer->placement(original_.size());
    }
    if (session_->tool != Tool::Gradient) {
        if ((session_->tool == Tool::Brush || session_->tool == Tool::Erase) &&
            (event->modifiers() & Qt::ShiftModifier) && lastBrushLayer_ == layer->id() &&
            lastBrushMask_ == paintMask()) {
            paintSegment(lastBrushPoint_, start_);
        } else {
            preparePaintArea(start_, start_);
            dab(start_);
        }
    }
    refreshStroke();
}
void Canvas::preparePaintArea(QPointF from, QPointF to) {
    if (session_->tool != Tool::Brush && session_->tool != Tool::Erase &&
        session_->tool != Tool::Clone && session_->tool != Tool::Heal)
        return;
    const auto radius = session_->brushSize / 2;
    auto area = QRectF(from, to).normalized().adjusted(-radius, -radius, radius, radius)
                    .intersected(QRectF(QPointF(), document_->size()));
    if (!session_->selection.isNull())
        area = area.intersected(strokeSelectionBounds_);
    if (area.isEmpty())
        return;
    auto layer = document_->active();
    const auto target = paintTarget(*layer, paintMask());
    const auto bounds = paintSurfaceBounds(target, area);
    if (bounds == target.image.rect())
        return;
    const auto oldExtent = layerExtent(*layer);
    const auto offset = growPaintSurface(*layer, paintMask(), bounds);
    const auto size = (paintMask() ? layer->mask : layer->image).size();
    auto pad = [&](const QImage &source) {
        QImage result(size, source.format());
        require(!result.isNull(), "Not enough memory for expanded stroke");
        result.fill(0);
        QPainter p(&result);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(offset, source);
        return result;
    };
    original_ = pad(original_);
    coverage_ = pad(coverage_);
    // Pixel indices and the placement changed, so the old incremental render caches cannot carry.
    strokePixels_ = {};
    carriedKey_ = (paintMask() ? layer->mask : layer->image).cacheKey();
    strokeArea_ |= oldExtent | layerExtent(*layer);
}
void Canvas::paintSegment(QPointF from, QPointF to) {
    preparePaintArea(from, to);
    const auto delta = to - from;
    const int steps = int(std::clamp(std::ceil(std::hypot(delta.x(), delta.y()) /
                                              std::max(.25, session_->brushSize * .1)),
                                   1.0, 200000.0));
    for (int i = 1; i <= steps; ++i)
        dab(from + delta * (double(i) / steps));
}
void Canvas::continuePaint(QMouseEvent *e) {
    auto point = toDocument(e->position());
    if ((session_->tool == Tool::Brush || session_->tool == Tool::Erase) &&
        session_->brushSmoothing > 0) {
        const auto delta = point - last_;
        const auto distance = std::hypot(delta.x(), delta.y());
        const auto radius = session_->brushSmoothing / std::max(.01, zoom);
        if (distance <= radius)
            return;
        point = last_ + delta * ((distance - radius) / distance);
    }
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
        paintSegment(last_, point);
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
        if (QLineF(start_, last_).length() < .001) {
            emit editCanceled();
            return;
        }
        auto target = paintTarget(*layer, paintMask());
        auto area = session_->selection.isNull() ? QRect(QPoint(), document_->size())
                                                : selectionBounds();
        if (!area.isEmpty()) {
            growPaintSurface(*layer, paintMask(), paintSurfaceBounds(target, area));
        }
        target = paintTarget(*layer, paintMask());
        original_ = target.image;
        auto placement = target.placement(original_.size());
        QImage painted = original_;
        painted.detach();
        QPainter p(&painted);
        p.setTransform(placement.inverted());
        QColor begin = session_->foreground,
               end = session_->gradientBackground ? session_->background : QColor(Qt::transparent);
        if (session_->gradientReverse)
            std::swap(begin, end);
        p.setOpacity(session_->brushOpacity);
        if (session_->gradientKind == 1) {
            QRadialGradient gradient(start_, QLineF(start_, last_).length());
            gradient.setColorAt(0, begin);
            gradient.setColorAt(1, end);
            p.fillRect(placement.mapRect(painted.rect()), gradient);
        } else {
            QLinearGradient gradient(start_, last_);
            gradient.setColorAt(0, begin);
            gradient.setColorAt(1, end);
            p.fillRect(placement.mapRect(painted.rect()), gradient);
        }
        p.end();
        auto filled = limitToSelection(original_, painted, selectionForLayer(target));
        paintChanged_ = filled != original_;
        if (paintMask())
            layer->mask = filled.convertToFormat(QImage::Format_Grayscale8);
        else {
            layer->image = filled;
            layer->metadata.remove("text");
            layer->metadata.remove("shape");
        }
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
                          float(session_->brushOpacity), session_->healingMode, 1) == 0,
                "Spot healing ran out of memory");
        paintChanged_ = image != original_;
        layer->image = image;
        layer->metadata.remove("text");
        layer->metadata.remove("shape");
    }
    if (session_->tool == Tool::Brush || session_->tool == Tool::Erase) {
        lastBrushPoint_ = last_;
        lastBrushLayer_ = layer->id();
        lastBrushMask_ = paintMask();
    }
    if (!paintChanged_ ||
        (layer->image == paintBefore_.image && layer->mask == paintBefore_.mask &&
         layer->metadata == paintBefore_.metadata)) {
        cloneOffset_ = cloneOffsetBefore_;
        cloneStrokeReady_ = cloneReadyBefore_;
        emit editCanceled();
    } else
        emit editFinished(session_->tool == Tool::Gradient ? "Gradient"
                          : session_->tool == Tool::Clone  ? "Clone Stamp"
                          : session_->tool == Tool::Heal   ? "Spot Healing"
                          : session_->tool == Tool::Blur   ? "Blur Stroke"
                          : paintMask()                    ? "Paint Mask"
                          : session_->tool == Tool::Erase  ? "Erase"
                                                           : "Brush Stroke");
    refresh();

    original_ = {};
    coverage_ = {};
    blurred_ = {};
    warp_.reset();
}
} // namespace compositor
