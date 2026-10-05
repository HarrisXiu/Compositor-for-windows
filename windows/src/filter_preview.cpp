// SPDX-License-Identifier: MIT
#include "filter_preview.h"
#include "camera_raw.h"
#include "canvas.h"
#include "editor.h"
#include "filters.h"
#include "render.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace compositor {
namespace {
constexpr double PreviewPixels = 16e6;
// The smallest step of 1, 1/√2, 1/2, … that is at least `wanted`, so the preview never has fewer
// pixels than the screen shows and a small change of zoom doesn't recompute it.
double ladder(double wanted) {
    if (wanted >= .93)
        return 1;
    const int steps = int(std::floor(-2 * std::log2(std::max(wanted, 1e-3))));
    return std::pow(2.0, -steps / 2.0);
}
bool reachBearing(const QString &kind) {
    return kind == "Gaussian Blur" || kind == "Motion Blur";
}
} // namespace

QJsonObject scaledPreviewSettings(const QString &kind, QJsonObject settings, double scale) {
    if (kind == "Bloom / Glow")
        settings["bloomRadius"] = std::max(.1, settings.value("bloomRadius").toDouble(24) * scale);
    else if (kind == "Tonal Contrast")
        settings["tonalRadius"] = std::max(.1, settings.value("tonalRadius").toDouble(16) * scale);
    else if (kind == "Gaussian Blur")
        settings["radius"] = settings.value("radius").toDouble(2) * scale;
    else if (kind == "Motion Blur")
        settings["distance"] = std::max(1.0, settings.value("distance").toDouble(10) * scale);
    else if (kind == "Camera Raw")
        settings["previewScale"] = std::min(1.0, scale);
    return settings;
}

// The selection as it lies over the layer, worked out on first use (by the background job).
struct FilterPreview::Coverage {
    QImage selection;
    Layer layer;
    QSize documentSize;
    QImage layerCoverage;
    bool done = false;
    const QImage &get() {
        if (!done) {
            done = true;
            if (!selection.isNull()) {
                Document shape;
                shape.metadata = {{"width", documentSize.width()}, {"height", documentSize.height()}};
                layerCoverage = layerSelection(shape, layer, selection);
            }
        }
        return layerCoverage;
    }
};

bool FilterPreview::supported(const QString &kind) {
    return kind != "Content-Aware Fill" && kind != "Invert";
}
FilterPreview::FilterPreview(EditorPage *page, const QString &kind, bool asAdjustment,
                             bool editExisting, QObject *parent)
    : QObject(parent), page_(page), canvas_(page->canvas), kind_(kind), asAdjustment_(asAdjustment),
      editExisting_(editExisting), runner_(this) {
    if (const auto *layer = page->document.active()) {
        layerId_ = layer->id();
        if (!asAdjustment) {
            image_ = layer->image;
            extent_ = layerExtent(*layer);
            grown_ = *layer;
            grownStates_[0] = grown_;
            coverage_ = std::make_shared<Coverage>();
            coverage_->selection = page->session.selection;
            coverage_->layer = *layer;
            coverage_->documentSize = page->document.size();
        }
    }
    adjustmentId_ = newId();
    viewTimer_.setSingleShot(true);
    viewTimer_.setInterval(150);
    connect(&viewTimer_, &QTimer::timeout, this, [this] {
        if (!stopped_ && enabled_ && haveSettings_ && !asAdjustment_ &&
            ladder(wantedScale()) != requestedScale_)
            schedule();
        emit busyChanged();
    });
    connect(canvas_, &Canvas::sessionChanged, this, &FilterPreview::viewChanged);
    connect(&runner_, &PreviewRunner::busyChanged, this, [this] { emit busyChanged(); });
    connect(&runner_, &PreviewRunner::failed, this, &FilterPreview::failed);
    connect(&runner_, &PreviewRunner::ready, this, [this](const QImage &result) {
        if (stopped_ || !enabled_ || result.isNull())
            return;
        // The layer the result was computed from: as it was, or padded for a blur.
        const auto state = grownStates_.value(result.text("grownMargin").toInt(), grown_);
        shownLayer_ = state;
        shownScale_ = double(result.width()) / std::max(1, state.image.width());
        shownImage_ = result;
        canvas_->setLivePreview(
            [id = layerId_, result, transform = state.metadata.value("transform"), mask = state.mask,
             placement = state.metadata.value("maskPlacement")](Document &d) {
                if (auto *layer = d.find(id)) {
                    layer->image = result;
                    layer->metadata["transform"] = transform;
                    layer->mask = mask;
                    if (placement.isObject())
                        layer->metadata["maskPlacement"] = placement;
                }
            },
            extent_, layerId_);
        ++shown_;
        emit shown();
    });
}
FilterPreview::~FilterPreview() {
    stop();
}
void FilterPreview::stop() {
    if (stopped_)
        return;
    stopped_ = true;
    viewTimer_.stop();
    runner_.cancel();
    canvas_->clearLivePreview();
    emit busyChanged();
}
void FilterPreview::setEnabled(bool enabled) {
    if (enabled == enabled_ || stopped_)
        return;
    enabled_ = enabled;
    if (!enabled_) {
        runner_.cancel();
        canvas_->clearLivePreview();
        emit busyChanged();
    } else if (haveSettings_)
        schedule();
}
void FilterPreview::update(const QJsonObject &settings) {
    settings_ = settings;
    haveSettings_ = true;
    if (!stopped_ && enabled_)
        schedule();
}
void FilterPreview::viewChanged() {
    if (!stopped_ && enabled_ && haveSettings_ && !asAdjustment_)
        viewTimer_.start();
}
QColor FilterPreview::shownColor(QPointF point) const {
    const auto &full = shownLayer_.image;
    if (shownImage_.isNull() || full.isNull())
        return {};
    const auto local = shownLayer_.placement(full.size()).inverted().map(point);
    const QPoint pixel(int(std::floor(local.x() * shownImage_.width() / full.width())),
                       int(std::floor(local.y() * shownImage_.height() / full.height())));
    if (!shownImage_.valid(pixel))
        return {};
    const auto color = shownImage_.pixelColor(pixel);
    return color.alpha() ? color : QColor();
}
double FilterPreview::wantedScale() const {
    if (image_.isNull())
        return 1;
    const double box = grown_.transform().value("size").toArray().at(0).toDouble(image_.width());
    const double screen = canvas_->screenScale() * box / std::max(1, image_.width());
    const double budget = std::sqrt(PreviewPixels / (double(image_.width()) * image_.height()));
    return std::clamp(std::min({1.0, screen, budget}), .03, 1.0);
}
void FilterPreview::showAdjustment() {
    try {
        const auto adjustment = makeAdjustment(kind_, settings_);
        std::function<void(Document &)> apply;
        QString edited;
        if (editExisting_) {
            edited = layerId_;
            apply = [id = layerId_, adjustment](Document &d) {
                if (auto *layer = d.find(id)) {
                    auto merged = layer->metadata.value("adjustment").toObject();
                    for (auto it = adjustment.begin(); it != adjustment.end(); ++it)
                        merged[it.key()] = it.value();
                    layer->metadata["adjustment"] = merged;
                }
            };
        } else {
            edited = adjustmentId_;
            Layer layer;
            const auto &id = adjustmentId_;
            layer.metadata = {{"id", id},
                              {"name", kind_},
                              {"isVisible", true},
                              {"opacity", 1},
                              {"blendMode", "Normal"},
                              {"transform", makeTransform(QRectF(QPointF(), page_->document.size()))},
                              {"adjustment", adjustment}};
            if (!page_->session.selection.isNull()) {
                layer.mask = page_->session.selection;
                layer.metadata["maskFile"] = id + ".mask.png";
            }
            apply = [layer, adjustment](Document &d) mutable {
                layer.metadata["adjustment"] = adjustment;
                d.layers.push_back(layer);
            };
        }
        // A blur reaches into neighboring tiles, so what was kept under it can't be reused.
        canvas_->setLivePreview(apply,
                                reachBearing(kind_) ? QRectF()
                                                    : QRectF(QPointF(), page_->document.size()),
                                edited);
        ++shown_;
        emit shown();
    } catch (const std::exception &e) {
        emit failed(QString::fromUtf8(e.what()));
    }
}
void FilterPreview::schedule() {
    if (asAdjustment_) {
        showAdjustment();
        return;
    }
    if (!supported(kind_) || image_.isNull())
        return;
    const int margin = int(std::ceil(filterMargin(kind_, settings_)));
    if (margin > grownMargin_) {
        try {
            Layer grown = grownStates_.value(0);
            growForFilter(grown, kind_, settings_);
            grown_ = grown;
            grownMargin_ = margin;
            grownStates_[margin] = grown;
            image_ = grown.image;
            extent_ = layerExtent(grown);
            auto coverage = std::make_shared<Coverage>();
            coverage->selection = coverage_->selection;
            coverage->documentSize = coverage_->documentSize;
            coverage->layer = grown;
            coverage_ = coverage;
        } catch (const std::exception &e) {
            emit failed(QString::fromUtf8(e.what()));
            return;
        }
    }
    requestedScale_ = ladder(wantedScale());
    const auto scale = requestedScale_;
    runner_.request([image = image_, coverage = coverage_, kind = kind_, grownMargin = grownMargin_,
                     settings = scaledPreviewSettings(kind_, settings_, scale), scale]() -> QImage {
        QImage source = image;
        if (scale < 1)
            source = image.scaled(std::max(1, int(std::lround(image.width() * scale))),
                                  std::max(1, int(std::lround(image.height() * scale))),
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        auto result = applyFilter(source, kind, settings);
        if (kind == "Camera Raw") {
            // The indicators are drawn over the canvas preview, never into the applied result.
            if (settings.value("previewSharpenMask").toBool())
                result = source;
            cameraRawPreviewOverlay(result, settings);
        }
        const auto &selected = coverage->get();
        if (!selected.isNull()) {
            auto mask = selected;
            if (mask.size() != source.size())
                mask = selected.scaled(source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            result = limitToSelection(source, result, mask);
        }
        result.setText("grownMargin", QString::number(grownMargin));
        return result;
    });
}
} // namespace compositor
