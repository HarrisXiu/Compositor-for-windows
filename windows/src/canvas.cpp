// SPDX-License-Identifier: MIT
#include "canvas.h"
#include "canvas_tools.h"
#include "canvas_text_editor.h"
#include "editable_layers.h"
#include "filters.h"
#include "render.h"
#include "shortcuts.h"
#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFocusEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <utility>
#include <vector>
extern "C" {
#include "HealPixels.h"
#include "WandPixels.h"
}

namespace compositor {
namespace {
constexpr int TileSize = 256;
quint64 tileKey(int level, int column, int row) {
    return quint64(level) << 56 | quint64(row) << 28 | quint64(column);
}
int keyLevel(quint64 key) {
    return int(key >> 56);
}
int keyRow(quint64 key) {
    return int(key >> 28 & 0xfffffff);
}
int keyColumn(quint64 key) {
    return int(key & 0xfffffff);
}
// The document at level n is drawn at 1/2^n of its size (rounding up), level 0 at full size.
QSize levelSize(QSize document, int level) {
    return {(document.width() + (1 << level) - 1) >> level,
            (document.height() + (1 << level) - 1) >> level};
}
int maxLevel(QSize document) {
    int level = 0;
    while (level < 30 && (std::max(document.width(), document.height()) >> (level + 1)) > 0)
        ++level;
    return level;
}
int tileCost(const QImage &image) {
    return int(std::min<qint64>(image.sizeInBytes() / 1024 + 1, INT_MAX));
}
struct TileJob {
    quint64 key;
    RenderArea area;
    QImage image, backdrop;
    bool newBackdrop = false;
    QString error;
};
} // namespace
struct RenderBatch {
    Document document;
    QSize full;
    quint64 generation = 0;
    std::vector<TileJob> jobs;
    std::atomic<bool> canceled{false};
};
Canvas::Canvas(Document *document, QWidget *parent) : Canvas(document, nullptr, parent) {}
Canvas::Canvas(Document *document, EditorSession *session, QWidget *parent)
    : QWidget(parent), document_(document), session_(session ? session : &fallbackSession_) {
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 200);
    session_->view = loadCanvasViewOptions();
    auto ants = new QTimer(this);
    ants->setInterval(100);
    connect(ants, &QTimer::timeout, this, [this] {
        if (isVisible() && !session_->selection.isNull()) {
            antsPhase_ = (antsPhase_ + 1) % 8;
            update();
        }
    });
    ants->start();
    stampAll();
    connect(&batchWatcher_, &QFutureWatcherBase::finished, this, &Canvas::finishBatch);
}
Canvas::~Canvas() {
    if (textEditor_) {
        textEditor_->changed = {};
        textEditor_->fontsChanged = {};
        textEditor_->apply = {};
        textEditor_->cancel = {};
        textEditor_->frameStarted = {};
        textEditor_->frameChanged = {};
    }
    if (batch_) {
        batch_->canceled = true;
        batchFuture_.waitForFinished();
    }
}
CanvasTool &Canvas::controller() {
    auto kind = temporaryPan_ ? Tool::Pan : session_->tool;
    if (!controller_ || controllerKind_ != kind) {
        controller_ = makeCanvasTool(kind, *this);
        controllerKind_ = kind;
    }
    return *controller_;
}
QRectF Canvas::canvasRect() const {
    const auto s = document_->size();
    const auto extent = QSizeF(s) * zoom;
    const double inset = session_->view.rulers ? 24 : 0;
    return QRectF(
        QPointF((width() + inset - extent.width()) / 2, (height() + inset - extent.height()) / 2) +
            pan_,
        extent);
}
QPointF Canvas::toDocument(QPointF point) const {
    return (point - canvasRect().topLeft()) / zoom;
}
void Canvas::stampAll() {
    const auto &d = *document_;
    stamps_.clear();
    stampOrder_.clear();
    stampSize_ = d.size();
    for (const auto &l : d.layers) {
        stampOrder_ << l.id();
        stamps_[l.id()] = {QJsonDocument(l.metadata).toJson(QJsonDocument::Compact),
                           l.image.cacheKey(), l.mask.cacheKey(), layerExtent(l)};
    }
    composite_ = {};
    renderError_.clear();
}
void Canvas::forgetTiles() {
    tiles_.clear();
    backdrops_.clear();
    composite_ = {};
    ++generation_;
    if (batch_)
        batch_->canceled = true;
}
void Canvas::refresh() {
    try {
        const auto &d = *document_;
        QStringList order;
        for (const auto &l : d.layers)
            order << l.id();
        if (d.size() != stampSize_ || order != stampOrder_) {
            forgetTiles();
            stampAll();
        } else {
            QRectF dirty;
            bool changed = false, everything = false, others = false;
            for (const auto &l : d.layers) {
                auto &stamp = stamps_[l.id()];
                auto metadata = QJsonDocument(l.metadata).toJson(QJsonDocument::Compact);
                if (metadata == stamp.metadata && l.image.cacheKey() == stamp.image &&
                    l.mask.cacheKey() == stamp.mask)
                    continue;
                changed = true;
                // A folder or an adjustment changes everything inside or under it.
                everything |= l.group() || l.metadata.value("adjustment").isObject();
                others |= l.id() != editedLayer_;
                const auto extent = layerExtent(l);
                dirty |= stamp.extent | extent;
                stamp = {metadata, l.image.cacheKey(), l.mask.cacheKey(), extent};
            }
            if (everything) {
                forgetTiles();
                stampAll();
            } else if (changed) {
                if (others)
                    backdrops_.clear();
                invalidate(dirty);
                composite_ = {};
                renderError_.clear();
            }
        }
    } catch (const std::exception &e) {
        forgetTiles();
        emit error(QString::fromUtf8(e.what()));
    }
    update();
}
void Canvas::refreshArea(const QRectF &area) {
    try {
        if (!area.isEmpty()) {
            invalidate(area);
            // The edit was to the active layer (or its mask) alone.
            if (auto l = document_->active(); l && stamps_.contains(l->id()))
                stamps_[l->id()] = {QJsonDocument(l->metadata).toJson(QJsonDocument::Compact),
                                    l->image.cacheKey(), l->mask.cacheKey(), layerExtent(*l)};
            composite_ = {};
        }
    } catch (const std::exception &e) {
        forgetTiles();
        emit error(QString::fromUtf8(e.what()));
    }
    update();
}
// Redraws what the dabs since the last call changed, updating the edited image's render caches
// over just that area.
void Canvas::refreshStroke() {
    if (auto layer = document_->active(); layer && !strokePixels_.isEmpty()) {
        const auto &target = paintMask() ? layer->mask : layer->image;
        try {
            carryRenderCaches(*layer, paintMask(), carriedKey_, strokePixels_);
        } catch (const std::exception &) {
            // The caches are rebuilt whole by the next render instead.
        }
        carriedKey_ = target.cacheKey();
        strokePixels_ = {};
    }
    refreshArea(std::exchange(strokeArea_, QRectF()));
}
// Drops the tiles a change to `area` (document pixels) shows in, including those whose pixels
// draw from it through a blur.
void Canvas::invalidate(const QRectF &area) {
    if (area.isEmpty())
        return;
    ++generation_;
    if (batch_)
        batch_->canceled = true;
    const auto doc = document_->size();
    QHash<int, int> reaches;
    const auto keys = tiles_.keys();
    for (auto key : keys) {
        const int level = keyLevel(key);
        const auto full = levelSize(doc, level);
        if (!reaches.contains(level))
            reaches[level] = renderReach(*document_, full);
        const int reach = reaches[level];
        const double sx = double(full.width()) / doc.width(),
                     sy = double(full.height()) / doc.height();
        const QRectF tile(keyColumn(key) * TileSize - reach, keyRow(key) * TileSize - reach,
                          TileSize + 2 * reach, TileSize + 2 * reach);
        if (QRectF(tile.x() / sx, tile.y() / sy, tile.width() / sx, tile.height() / sy)
                .intersects(area))
            tiles_.remove(key);
    }
}
// While one layer is edited repeatedly, each tile keeps what lies under it, so redrawing the
// tile composites only that layer and those above.
void Canvas::beginLayerEdit(const QString &id) {
    editedLayer_ = id;
    backdrops_.clear();
    // Edits redraw on the UI thread, so each dab shows at once.
    if (batch_)
        batch_->canceled = true;
}
void Canvas::endLayerEdit() {
    editedLayer_.clear();
    backdrops_.clear();
}
QImage Canvas::fullComposite() {
    if (composite_.isNull())
        composite_ = renderDocument(*document_);
    return composite_;
}
void Canvas::fit() {
    auto s = document_->size();
    if (s.isEmpty())
        return;
    fitted_ = true;
    zoom = std::min((width() - 64.0) / s.width(), (height() - 64.0) / s.height());
    zoom = std::max(0.01, zoom);
    pan_ = {};
    refresh();
}
void Canvas::setTool(Tool value) {
    if (value != session_->tool) {
        finishTextEditing(true);
        cancelInteraction();
        session_->cropFrame = {};
        if (value == Tool::Crop && !session_->selection.isNull())
            session_->cropFrame = selectionBounds();
    }
    if (value != Tool::Wand)
        composite_ = {};
    session_->tool = value;
    setCursor(session_->tool == Tool::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
    update();
    emit sessionChanged();
}
void Canvas::applyCropFrame() {
    const auto frame = session_->cropFrame.normalized();
    if (frame.width() < 1 || frame.height() < 1)
        return;
    cancelInteraction();
    const QRect bounds(
        QPoint(int(std::floor(frame.left())), int(std::floor(frame.top()))),
        QPoint(int(std::ceil(frame.right())) - 1, int(std::ceil(frame.bottom())) - 1));
    session_->cropFrame = {};
    emit cropRequested(bounds);
    update();
}
void Canvas::cancelCropFrame() {
    cancelInteraction();
    session_->cropFrame = {};
    update();
}
void Canvas::mouseDoubleClickEvent(QMouseEvent *event) {
    if ((session_->tool == Tool::Text || session_->tool == Tool::Move) &&
        event->button() == Qt::LeftButton && !textLayerAt(toDocument(event->position())).isEmpty()) {
        const auto id = textLayerAt(toDocument(event->position()));
        try {
            finishTextEditing(true);
            cancelInteraction();
            beginTextEditing(id);
        } catch (const std::exception &error) {
            cancelInteraction();
            emit this->error(QString::fromUtf8(error.what()));
        }
        event->accept();
    } else if (session_->tool == Tool::Crop && event->button() == Qt::LeftButton) {
        applyCropFrame();
        event->accept();
    } else
        QWidget::mouseDoubleClickEvent(event);
}
void Canvas::cancelInteraction() {
    finishTextEditing(false);
    if (!dragging_)
        return;
    dragging_ = false;
    guideDrag_ = -1;
    endTransform();
    if (session_->tool == Tool::Crop && !temporaryPan_)
        session_->cropFrame = session_->cropBeforeGesture;
    warp_.reset();
    original_ = {};
    coverage_ = {};
    blurred_ = {};
    lasso_ = {};
    if (!paintBefore_.id().isEmpty()) {
        cloneOffset_ = cloneOffsetBefore_;
        cloneStrokeReady_ = cloneReadyBefore_;
        paintBefore_ = {};
    }
    cloneSample_ = {};
    endLayerEdit();
    emit editCanceled();
    update();
}
void Canvas::replaceSelection(const QImage &mask, const QString &label) {
    require(mask.isNull() ||
                (mask.size() == document_->size() && mask.format() == QImage::Format_Grayscale8),
            "Selection must match the canvas size");
    if (mask == session_->selection)
        return;
    auto before = session_->selection;
    session_->selection = mask;
    emit selectionEdited(label, before, mask);
    emit selectionChanged();
    update();
}
void Canvas::clearSelection() {
    cancelInteraction();
    replaceSelection({}, "Deselect");
}
void Canvas::selectAll() {
    cancelInteraction();
    QImage next(document_->size(), QImage::Format_Grayscale8);
    if (next.isNull()) {
        emit error("Not enough memory for selection");
        return;
    }
    next.fill(255);
    replaceSelection(next, "Select All");
}
void Canvas::invertSelection() {
    cancelInteraction();
    if (session_->selection.isNull()) {
        selectAll();
        return;
    }
    auto next = session_->selection;
    next.detach();
    for (int y = 0; y < next.height(); ++y) {
        auto p = next.scanLine(y);
        for (int x = 0; x < next.width(); ++x)
            p[x] = 255 - p[x];
    }
    replaceSelection(next, "Inverse Selection");
}
void Canvas::featherSelection(double radius) {
    cancelInteraction();
    if (session_->selection.isNull())
        return;
    auto rgba = session_->selection.convertToFormat(QImage::Format_RGBA8888);
    replaceSelection(gaussianBlur(rgba, radius).convertToFormat(QImage::Format_Grayscale8),
                     "Feather Selection");
}
QRect Canvas::selectionBounds() const {
    if (session_->selection.isNull())
        return {};
    int x0 = session_->selection.width(), y0 = session_->selection.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < session_->selection.height(); ++y) {
        auto p = session_->selection.constScanLine(y);
        for (int x = 0; x < session_->selection.width(); ++x)
            if (p[x]) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    }
    return x1 >= x0 ? QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1) : QRect();
}
QImage Canvas::selectionForLayer(const Layer &layer) const {
    return layerSelection(*document_, layer, session_->selection);
}
void Canvas::paintEvent(QPaintEvent *) {
    syncTextEditor();
    QPainter p(this);
    p.fillRect(rect(), QColor(29, 31, 36));
    auto r = canvasRect();
    p.save();
    p.setClipRect(r);
    const int tile = 12;
    auto clipped = r.intersected(rect());
    for (int y = int(clipped.top()); y <= clipped.bottom(); y += tile)
        for (int x = int(clipped.left()); x <= clipped.right(); x += tile)
            p.fillRect(x, y, tile, tile,
                       ((x / tile + y / tile) & 1) ? QColor(88, 90, 95) : QColor(112, 114, 119));
    drawDocument(p, r);
    drawPixelGrid(p, r);
    const auto guides = document_->metadata.value("guides").toArray();
    p.setPen(QPen(session_->view.guideColor, 1));
    if (session_->view.guides)
        for (const auto &v : guides) {
            auto g = v.toObject();
            double position = g.value("position").toDouble() * zoom;
            if (g.value("axis") == "horizontal")
                p.drawLine(QPointF(r.left(), r.top() + position),
                           QPointF(r.right(), r.top() + position));
            else
                p.drawLine(QPointF(r.left() + position, r.top()),
                           QPointF(r.left() + position, r.bottom()));
        }
    p.restore();
    p.setPen(QColor(5, 6, 8));
    p.drawRect(r);
    if (!session_->selection.isNull()) {
        if (session_->selection.cacheKey() != selectionOutlineKey_) {
            selectionOutlineKey_ = session_->selection.cacheKey();
            selectionOutline_ = QPainterPath();
            auto mask = session_->selection;
            double scale = 1;
            if (std::max(mask.width(), mask.height()) > 2048) {
                mask = mask.scaled(2048, 2048, Qt::KeepAspectRatio, Qt::FastTransformation);
                scale = double(session_->selection.width()) / mask.width();
            }
            auto selected = [&](int x, int y) {
                return mask.valid(x, y) && mask.constScanLine(y)[x] >= 128;
            };
            auto edge = [&](QPointF a, QPointF b) {
                selectionOutline_.moveTo(a * scale);
                selectionOutline_.lineTo(b * scale);
            };
            auto runs = [&](int length, auto boundary, auto coordinate) {
                int start = -1;
                for (int i = 0; i <= length; ++i) {
                    const bool inside = i < length && boundary(i);
                    if (inside && start < 0)
                        start = i;
                    if (!inside && start >= 0) {
                        edge(coordinate(start), coordinate(i));
                        start = -1;
                    }
                }
            };
            for (int y = 0; y < mask.height(); ++y) {
                runs(
                    mask.width(), [&](int x) { return selected(x, y) && !selected(x, y - 1); },
                    [&](int x) { return QPointF(x, y); });
                runs(
                    mask.width(), [&](int x) { return selected(x, y) && !selected(x, y + 1); },
                    [&](int x) { return QPointF(x, y + 1); });
            }
            for (int x = 0; x < mask.width(); ++x) {
                runs(
                    mask.height(), [&](int y) { return selected(x, y) && !selected(x - 1, y); },
                    [&](int y) { return QPointF(x, y); });
                runs(
                    mask.height(), [&](int y) { return selected(x, y) && !selected(x + 1, y); },
                    [&](int y) { return QPointF(x + 1, y); });
            }
        }
        if (!selectionOutline_.isEmpty()) {
            QTransform map;
            map.translate(r.left(), r.top());
            map.scale(zoom, zoom);
            auto outline = map.map(selectionOutline_);
            p.setPen(QPen(Qt::black, 1));
            p.drawPath(outline);
            QPen ants(Qt::white, 1, Qt::CustomDashLine);
            ants.setDashPattern({4, 4});
            ants.setDashOffset(antsPhase_);
            p.setPen(ants);
            p.drawPath(outline);
        }
    }
    controller().paintOverlay(p);
    drawCanvasOverlay(p);
}
// Draws the document into `target` from tiles rendered at the level that matches the zoom: at
// least as many pixels as the screen shows, so drawing only shrinks a level by up to half, or
// enlarges full size pixel for pixel. Only tiles that are on screen are rendered.
void Canvas::drawDocument(QPainter &p, const QRectF &target) {
    Document display = *document_;
    if (textEditor_)
        if (auto layer = display.find(textLayerID_))
            layer->metadata["isVisible"] = false;
    const auto doc = document_->size();
    const QRectF shown = target.intersected(QRectF(rect()));
    if (doc.isEmpty() || shown.isEmpty())
        return;
    const double device = zoom * devicePixelRatioF();
    const int level =
        std::clamp(device < 1 ? int(std::floor(std::log2(1 / device))) : 0, 0, maxLevel(doc));
    const auto full = levelSize(doc, level);
    const double sx = double(full.width()) / doc.width(), sy = double(full.height()) / doc.height();
    const QRect pixels =
        QRectF((shown.left() - target.left()) / zoom * sx, (shown.top() - target.top()) / zoom * sy,
               shown.width() / zoom * sx, shown.height() / zoom * sy)
            .toAlignedRect()
            .intersected(QRect(QPoint(), full));
    if (pixels.isEmpty())
        return;
    const int column0 = pixels.left() / TileSize, column1 = pixels.right() / TileSize,
              row0 = pixels.top() / TileSize, row1 = pixels.bottom() / TileSize;
    std::vector<TileJob> jobs;
    QHash<quint64, QImage> ready;
    for (int row = row0; row <= row1; ++row)
        for (int column = column0; column <= column1; ++column) {
            const auto key = tileKey(level, column, row);
            if (auto tile = tiles_.object(key)) {
                ready[key] = *tile;
                continue;
            }
            TileJob job{key,
                        {full,
                         QRect(column * TileSize, row * TileSize, TileSize, TileSize)
                             .intersected(QRect(QPoint(), full)),
                         true, false}};
            if (!editedLayer_.isEmpty())
                if (auto backdrop = backdrops_.object(key))
                    job.backdrop = *backdrop;
            jobs.push_back(std::move(job));
        }
    if (!jobs.empty() && editedLayer_.isEmpty()) {
        // Rendered in the background from a copy of the document; a batch that is out of date
        // is canceled, and the next paint after it ends starts one for what is still missing.
        if (batch_) {
            if (batch_->full != full)
                batch_->canceled = true;
        } else {
            auto batch = std::make_shared<RenderBatch>();
            batch->document = display;
            batch->full = full;
            batch->generation = generation_;
            batch->jobs = std::move(jobs);
            batch_ = batch;
            batchFuture_ = QtConcurrent::run([batch] {
                try {
                    prepareRender(batch->document, batch->full, true);
                } catch (const std::exception &) {
                    // Each tile reports its own failure.
                }
                QtConcurrent::blockingMap(batch->jobs, [&batch](TileJob &job) {
                    if (batch->canceled)
                        return;
                    try {
                        job.image = renderArea(batch->document, job.area);
                    } catch (const std::exception &e) {
                        job.error = QString::fromUtf8(e.what());
                    }
                });
            });
            batchWatcher_.setFuture(batchFuture_);
        }
        jobs.clear();
    }
    if (!jobs.empty()) {
        QString failure;
        try {
            prepareRender(display, full, true);
        } catch (const std::exception &e) {
            failure = QString::fromUtf8(e.what());
        }
        QtConcurrent::blockingMap(jobs, [this, &display](TileJob &job) {
            try {
                if (editedLayer_.isEmpty()) {
                    job.image = renderArea(display, job.area);
                    return;
                }
                if (job.backdrop.isNull()) {
                    job.backdrop = renderBackdrop(display, job.area, editedLayer_);
                    job.newBackdrop = true;
                }
                job.image = renderOver(display, job.area, editedLayer_, job.backdrop);
            } catch (const std::exception &e) {
                job.error = QString::fromUtf8(e.what());
            }
        });
        for (auto &job : jobs) {
            if (!job.error.isEmpty()) {
                failure = job.error;
                continue;
            }
            if (job.newBackdrop)
                backdrops_.insert(job.key, new QImage(job.backdrop), tileCost(job.backdrop));
            tiles_.insert(job.key, new QImage(job.image), tileCost(job.image));
            ready[job.key] = job.image;
        }
        reportRenderError(failure);
    }
    // One image for all the tiles shown, so scaling it can't leave seams between them.
    const auto span = QRect(column0 * TileSize, row0 * TileSize, (column1 - column0 + 1) * TileSize,
                            (row1 - row0 + 1) * TileSize)
                          .intersected(QRect(QPoint(), full));
    QImage stitched(span.size(), QImage::Format_RGBA8888_Premultiplied);
    if (stitched.isNull())
        return;
    stitched.fill(Qt::transparent);
    {
        QPainter s(&stitched);
        s.setCompositionMode(QPainter::CompositionMode_Source);
        for (int row = row0; row <= row1; ++row)
            for (int column = column0; column <= column1; ++column) {
                const QRect tile = QRect(column * TileSize, row * TileSize, TileSize, TileSize)
                                       .intersected(QRect(QPoint(), full));
                if (auto it = ready.constFind(tileKey(level, column, row)); it != ready.cend()) {
                    s.drawImage(tile.topLeft() - span.topLeft(), it.value());
                    continue;
                }
                // Not rendered yet: shown meanwhile from tiles of a coarser level, scaled up, or
                // of the next finer one, scaled down.
                s.save();
                s.setClipRect(tile.translated(-span.topLeft()));
                s.setRenderHint(QPainter::SmoothPixmapTransform);
                for (int other : {level + 1, level + 2, level + 3, level + 4, level - 1}) {
                    if (other < 0 || other > maxLevel(doc))
                        continue;
                    const auto otherFull = levelSize(doc, other);
                    const double fx = double(full.width()) / otherFull.width(),
                                 fy = double(full.height()) / otherFull.height();
                    const auto there =
                        QRectF(tile.x() / fx, tile.y() / fy, tile.width() / fx, tile.height() / fy)
                            .toAlignedRect()
                            .intersected(QRect(QPoint(), otherFull));
                    bool found = false;
                    for (int r = there.top() / TileSize; r <= there.bottom() / TileSize; ++r)
                        for (int c = there.left() / TileSize; c <= there.right() / TileSize; ++c)
                            if (auto cached = tiles_.object(tileKey(other, c, r))) {
                                found = true;
                                s.drawImage(QRectF((c * TileSize * fx) - span.x(),
                                                   (r * TileSize * fy) - span.y(),
                                                   cached->width() * fx, cached->height() * fy),
                                            *cached);
                            }
                    if (found)
                        break;
                }
                s.restore();
            }
    }
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, device < sx);
    p.drawImage(QRectF(target.left() + span.x() / sx * zoom, target.top() + span.y() / sy * zoom,
                       span.width() / sx * zoom, span.height() / sy * zoom),
                stitched);
    p.restore();
}
void Canvas::finishBatch() {
    // The watcher can still report a batch that waitForRendering has already taken.
    if (!batch_ || !batchFuture_.isFinished())
        return;
    auto batch = std::exchange(batch_, nullptr);
    if (!batch->canceled && batch->generation == generation_) {
        QString failure;
        for (auto &job : batch->jobs) {
            if (!job.error.isEmpty())
                failure = job.error;
            else if (!job.image.isNull())
                tiles_.insert(job.key, new QImage(job.image), tileCost(job.image));
        }
        reportRenderError(failure);
    }
    update();
}
void Canvas::waitForRendering() {
    if (!batch_)
        return;
    batchFuture_.waitForFinished();
    finishBatch();
}
// Reported after the current paint, once per message, so a failure can't recurse into painting.
void Canvas::reportRenderError(const QString &message) {
    if (message.isEmpty() || message == renderError_)
        return;
    renderError_ = message;
    QMetaObject::invokeMethod(this, [this, message] { emit error(message); }, Qt::QueuedConnection);
}
// At 800% and above, a line between every pair of pixels.
void Canvas::drawPixelGrid(QPainter &p, const QRectF &target) {
    const QRectF shown = target.intersected(QRectF(rect()));
    if (zoom < 8 || shown.isEmpty())
        return;
    QVector<QLineF> lines;
    for (int x = int(std::ceil((shown.left() - target.left()) / zoom)),
             end = int(std::floor((shown.right() - target.left()) / zoom));
         x <= end; ++x)
        lines << QLineF(target.left() + x * zoom, shown.top(), target.left() + x * zoom,
                        shown.bottom());
    for (int y = int(std::ceil((shown.top() - target.top()) / zoom)),
             end = int(std::floor((shown.bottom() - target.top()) / zoom));
         y <= end; ++y)
        lines << QLineF(shown.left(), target.top() + y * zoom, shown.right(),
                        target.top() + y * zoom);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(QColor(128, 128, 128, 90), 0));
    p.drawLines(lines);
    p.restore();
}
void Canvas::drawGesture(QPainter &p) {
    auto r = canvasRect();
    if (dragging_ && (session_->tool == Tool::RectangleSelect ||
                      session_->tool == Tool::EllipseSelect || session_->tool == Tool::Crop ||
                      session_->tool == Tool::Rectangle || session_->tool == Tool::Ellipse ||
                      session_->tool == Tool::Line || session_->tool == Tool::Gradient ||
                      session_->tool == Tool::Text)) {
        QRectF outline(r.topLeft() + start_ * zoom, r.topLeft() + last_ * zoom);
        p.setPen(QPen(Qt::white, 1, Qt::DashLine));
        if (session_->tool == Tool::EllipseSelect || session_->tool == Tool::Ellipse)
            p.drawEllipse(outline.normalized());
        else if (session_->tool == Tool::Line || session_->tool == Tool::Gradient)
            p.drawLine(outline.topLeft(), outline.bottomRight());
        else
            p.drawRect(outline.normalized());
    }
    if (dragging_ && session_->tool == Tool::Lasso) {
        QTransform map;
        map.translate(r.x(), r.y());
        map.scale(zoom, zoom);
        p.setPen(QPen(Qt::white, 1, Qt::DashLine));
        p.drawPath(map.map(lasso_));
    }
}
void Canvas::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    if (!fitted_)
        fit();
}
void Canvas::dab(QPointF point) {
    auto layer = document_->active();
    if (!layer)
        return;
    const auto reach = session_->brushSize / 2;
    if (!QRectF(QPointF(), document_->size()).adjusted(-reach, -reach, reach, reach).contains(point))
        return;
    QImage &target = paintMask() ? layer->mask : layer->image;
    auto placement = layer->placement(target.size());
    if (paintMask() && layer->metadata.value("maskPlacement").isObject()) {
        Layer maskLayer = *layer;
        maskLayer.metadata["transform"] = layer->metadata.value("maskPlacement");
        placement = maskLayer.placement(target.size());
    }
    const auto inverse = placement.inverted();
    const auto pixel = inverse.map(point);
    const double sx = std::hypot(placement.m11(), placement.m12()),
                 sy = std::hypot(placement.m21(), placement.m22());
    double rx = session_->brushSize / (2 * std::max(0.001, sx)),
           ry = session_->brushSize / (2 * std::max(0.001, sy));
    const auto bounds = QRectF(pixel.x() - rx, pixel.y() - ry, 2 * rx, 2 * ry)
                            .intersected(QRectF(target.rect())).toAlignedRect();
    if (bounds.isEmpty())
        return;
    const int x0 = bounds.left(), x1 = bounds.right(), y0 = bounds.top(), y1 = bounds.bottom();
    const auto cloneInverse = cloneSamplePlacement_.inverted();
    target.detach();
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            double radius = std::hypot((x + 0.5 - pixel.x()) / rx, (y + 0.5 - pixel.y()) / ry);
            if (radius > 1)
                continue;
            double amount = radius <= session_->hardness
                                ? 1
                                : (1 - radius) / std::max(0.001, 1 - session_->hardness);
            auto docPoint = placement.map(QPointF(x + 0.5, y + 0.5));
            if (!QRectF(QPointF(), document_->size()).contains(docPoint))
                continue;
            if (!session_->selection.isNull()) {
                int px = int(std::floor(docPoint.x())), py = int(std::floor(docPoint.y()));
                if (!session_->selection.valid(px, py))
                    continue;
                amount *= session_->selection.constScanLine(py)[px] / 255.0;
            }
            auto mask = coverage_.scanLine(y) + x;
            *mask = std::max(*mask, uchar(std::clamp(std::lround(amount * 255), 0L, 255L)));
            amount = *mask / 255.0 * session_->brushOpacity;
            if (session_->tool == Tool::Heal)
                continue;
            if (paintMask()) {
                auto v = session_->tool == Tool::Blur    ? blurred_.constScanLine(y)[x]
                         : session_->tool == Tool::Erase ? 0
                                                         : qGray(session_->foreground.rgb());
                auto &value = target.scanLine(y)[x];
                auto next = uchar(std::lround(original_.constScanLine(y)[x] * (1 - amount) +
                                             v * amount));
                paintChanged_ |= value != next;
                value = next;
                continue;
            }
            auto p = target.scanLine(y) + x * 4;
            auto base = original_.constScanLine(y) + x * 4;
            const std::array<uchar, 4> before{p[0], p[1], p[2], p[3]};
            if (session_->tool == Tool::Blur) {
                auto source = blurred_.constScanLine(y) + x * 4;
                for (int c = 0; c < 4; ++c)
                    p[c] = uchar(std::lround(base[c] * (1 - amount) + source[c] * amount));
            } else if (session_->tool == Tool::Erase) {
                for (int c = 0; c < 4; ++c)
                    p[c] = uchar(std::lround(base[c] * (1 - amount)));
            } else if (session_->tool == Tool::Clone) {
                auto q = cloneInverse.map(docPoint + cloneOffset_);
                int cx = int(std::floor(q.x())), cy = int(std::floor(q.y()));
                if (!cloneSample_.valid(cx, cy))
                    continue;
                auto source = cloneSample_.constScanLine(cy) + cx * 4;
                const double sourceAlpha = source[3] / 255.0 * amount;
                for (int c = 0; c < 4; ++c)
                    p[c] = uchar(std::lround(base[c] * (1 - sourceAlpha) + source[c] * amount));
            } else {
                double alpha = amount * session_->foreground.alphaF();
                for (int c = 0; c < 3; ++c) {
                    int v = c == 0   ? session_->foreground.red()
                            : c == 1 ? session_->foreground.green()
                                     : session_->foreground.blue();
                    p[c] = uchar(std::lround(base[c] * (1 - alpha) + v * alpha));
                }
                p[3] = uchar(std::lround(base[3] * (1 - alpha) + 255 * alpha));
            }
            paintChanged_ |= !std::equal(before.begin(), before.end(), p);
        }
    if (!paintMask() && session_->tool != Tool::Heal) {
        layer->metadata.remove("text");
        layer->metadata.remove("shape");
    }
    // Spot Healing only gathers coverage here; its pixels change when the stroke ends.
    if (session_->tool != Tool::Heal && x1 >= x0 && y1 >= y0) {
        strokePixels_ |= QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        strokeArea_ |= placement.mapRect(QRectF(x0, y0, x1 - x0 + 1, y1 - y0 + 1))
                           .adjusted(-strokeReach_, -strokeReach_, strokeReach_, strokeReach_);
    }
    emit edited();
}
void Canvas::finishSelection() {
    QImage mask(document_->size(), QImage::Format_Grayscale8);
    require(!mask.isNull(), "Not enough memory for selection");
    mask.fill(0);
    QPainter p(&mask);
    p.setRenderHint(QPainter::Antialiasing,
                    session_->selectionAntialiased && session_->tool != Tool::RectangleSelect);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    auto r = QRectF(start_, last_).normalized();
    if (session_->tool == Tool::EllipseSelect)
        p.drawEllipse(r);
    else if (session_->tool == Tool::Lasso) {
        lasso_.closeSubpath();
        p.drawPath(lasso_);
    } else
        p.drawRect(r);
    p.end();
    if (!priorSelection_.isNull() &&
        (selectionModifiers_ & (Qt::ShiftModifier | Qt::AltModifier))) {
        for (int y = 0; y < mask.height(); ++y) {
            auto n = mask.scanLine(y);
            auto old = priorSelection_.constScanLine(y);
            for (int x = 0; x < mask.width(); ++x)
                n[x] = ((selectionModifiers_ & (Qt::AltModifier | Qt::ShiftModifier)) ==
                        (Qt::AltModifier | Qt::ShiftModifier))
                           ? uchar(int(old[x]) * int(n[x]) / 255)
                       : (selectionModifiers_ & Qt::AltModifier)
                           ? uchar(old[x] * (1 - n[x] / 255.0))
                           : std::max(old[x], n[x]);
        }
    }
    replaceSelection(mask, "Change Selection");
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || dragging_)
        return;
    finishTextEditing(true);
    setFocus();
    start_ = last_ = toDocument(e->position());
    hover_ = e->position();
    hovered_ = true;
    try {
        if (beginOverlayEdit(e))
            return;
        if (brushTool() && (e->modifiers() & Qt::AltModifier) && session_->tool != Tool::Clone) {
            auto pixel = QPoint(int(std::floor(start_.x())), int(std::floor(start_.y())));
            if (QRect(QPoint(), document_->size()).contains(pixel))
                emit colorPicked(sampleColor(start_, session_->pickerSize));
            return;
        }
        controller().press(e);
    } catch (const std::exception &ex) {
        cancelInteraction();
        emit error(QString::fromUtf8(ex.what()));
    }
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    hover_ = e->position();
    hovered_ = true;
    temporaryPicker_ =
        brushTool() && session_->tool != Tool::Clone && (e->modifiers() & Qt::AltModifier);
    if (session_->tool == Tool::Eyedropper || temporaryPicker_) {
        auto point = toDocument(hover_);
        QPoint pixel(int(std::floor(point.x())), int(std::floor(point.y())));
        if (QRect(QPoint(), document_->size()).contains(pixel)) {
            try {
                hoverColor_ = sampleColor(point, session_->pickerSize);
            } catch (const std::exception &) {
                hoverColor_ = Qt::transparent;
            }
        } else
            hoverColor_ = Qt::transparent;
    }
    setCursor(temporaryPan_ || session_->tool == Tool::Pan ? Qt::OpenHandCursor
              : brushTool() && !temporaryPicker_           ? Qt::BlankCursor
                                                           : Qt::CrossCursor);
    update();
    if (!dragging_)
        return;
    try {
        if (moveOverlayEdit(e))
            return;
        controller().move(e);
    } catch (const std::exception &ex) {
        cancelInteraction();
        emit error(QString::fromUtf8(ex.what()));
    }
}
void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (!dragging_ || e->button() != Qt::LeftButton)
        return;
    const auto previous = last_;
    last_ = toDocument(e->position());
    try {
        if (!original_.isNull() && !warp_ && brushTool()) {
            paintSegment(previous, last_);
            refreshStroke();
        }
        if (!finishOverlayEdit(e))
            controller().release(e);
    } catch (const std::exception &ex) {
        emit error(QString::fromUtf8(ex.what()));
    }
    endLayerEdit();
    dragging_ = false;
    original_ = {};
    coverage_ = {};
    blurred_ = {};
    warp_.reset();
    paintBefore_ = {};
    cloneSample_ = {};
    update();
}
void Canvas::keyPressEvent(QKeyEvent *e) {
    try {
        if (handleCanvasKey(e)) {
            e->accept();
            return;
        }
    } catch (const std::exception &ex) {
        cancelInteraction();
        emit error(QString::fromUtf8(ex.what()));
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}
void Canvas::keyReleaseEvent(QKeyEvent *e) {
    if ((temporaryPan_ && e->key() == panPhysicalKey_) && !e->isAutoRepeat()) {
        cancelInteraction();
        temporaryPan_ = false;
        setCursor(session_->tool == Tool::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
        e->accept();
    } else if (!controller().keyRelease(e)) {
        QWidget::keyReleaseEvent(e);
    }
}
void Canvas::focusOutEvent(QFocusEvent *e) {
    // Native inline input and its toolbar/dialogs take focus from the canvas.
    if (!textEditor_)
        cancelInteraction();
    temporaryPan_ = false;
    setCursor(session_->tool == Tool::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
    QWidget::focusOutEvent(e);
}
void Canvas::wheelEvent(QWheelEvent *e) {
    auto point = toDocument(e->position());
    zoom = std::clamp(zoom * std::pow(1.15, e->angleDelta().y() / 120.0), 0.01, 64.0);
    auto now = canvasRect().topLeft() + point * zoom;
    pan_ += e->position() - now;
    update();
    emit sessionChanged();
}
void Canvas::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void Canvas::dropEvent(QDropEvent *e) {
    QStringList paths;
    for (const auto &url : e->mimeData()->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    if (!paths.isEmpty()) {
        emit filesDropped(paths);
        e->acceptProposedAction();
    }
}
} // namespace compositor
