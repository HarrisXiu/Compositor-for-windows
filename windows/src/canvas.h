// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include "editor_session.h"
#include "warp_brush.h"
#include <QCache>
#include <QElapsedTimer>
#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QPainterPath>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>

namespace compositor {
struct RenderBatch;
class CanvasTool;
class PaintTool;
class SelectionTool;
class ShapeTool;
class MoveTool;
class PanTool;
class EyedropperTool;
class WandTool;
class TextTool;
class CropTool;
class CanvasTextEditor;
class Canvas : public QWidget {
    Q_OBJECT
  public:
    explicit Canvas(Document *document, QWidget *parent = nullptr);
    Canvas(Document *document, EditorSession *session, QWidget *parent);
    ~Canvas() override;
    EditorSession &session() {
        return *session_;
    }
    const EditorSession &session() const {
        return *session_;
    }
    bool paintMask() const {
        return session_->target == EditTarget::Mask;
    }
    double zoom = 1;
    // Redraws whatever changed in the document since the last refresh.
    void refresh();
    // Redraws `area` (document pixels) after an edit confined to it, such as a brush dab.
    void refreshArea(const QRectF &area);
    // The document rendered at full size, kept until it changes.
    QImage fullComposite();
    // Whether tiles are being rendered in the background, and waiting for them (for tests).
    bool rendering() const {
        return batch_ != nullptr;
    }
    void waitForRendering();
    void fit();
    void clearSelection();
    // Ctrl+T with a selection: the selected pixels float on a layer of their own, edited with the
    // transform handles; Enter merges them back, Escape restores the document exactly. The whole
    // thing is one undo step.
    bool canTransformSelection() const;
    void beginSelectionTransform();
    void commitFloatingSelection();
    void cancelFloatingSelection();
    bool hasFloatingSelection() const {
        return floating_.has_value();
    }
    // Shows an edit that is not made yet. `apply` changes the copy of the document the canvas draws,
    // never the document itself; `dirty` is the document area it changes (empty: everything) and
    // `editedLayer` the one layer it changes, so redrawing composites only that layer and those above.
    void setLivePreview(std::function<void(Document &)> apply, const QRectF &dirty = {},
                        const QString &editedLayer = {});
    void clearLivePreview();
    bool hasLivePreview() const {
        return bool(livePreview_);
    }
    // While a dialog that doesn't block the window edits the document, the canvas only pans and
    // zooms, apart from what a pick handler asks for.
    void setInputLocked(bool locked);
    bool inputLocked() const {
        return inputLocked_;
    }
    struct PickEvent {
        enum class Phase { Press, Move, Release } phase = Phase::Press;
        QPointF point; // On the document, in pixels.
        Qt::KeyboardModifiers modifiers;
    };
    // Mouse gestures on the canvas go to the handler instead of the current tool (a locked canvas
    // pans as ever); Escape and an empty handler end it.
    void setPickHandler(std::function<void(const PickEvent &)> handler);
    // How many screen pixels one document pixel takes.
    double screenScale() const {
        return zoom * devicePixelRatioF();
    }
    void selectAll();
    void invertSelection();
    void replaceSelection(const QImage &mask, const QString &label);
    void featherSelection(double radius);
    void resizeSelection(int radius, bool expand);
    void cancelInteraction();
    void beginTextEditing(const QString &id = {}, QRectF bounds = {});
    void finishTextEditing(bool apply = true);
    bool textEditing() const { return textEditor_ != nullptr; }
    void setAiPicking(bool enabled);
    void setAiPoints(const QVector<QPointF> &points, const QVector<int> &labels);
    void applyCropFrame();
    void cancelCropFrame();
    QRect selectionBounds() const;
    void setTool(Tool value);
    void cycleToolMode();
    void zoomTo(double value, QPointF anchor = {});
    double snapValue(double value, bool horizontal, Qt::KeyboardModifiers modifiers = {},
                     const QSet<QString> &exclude = {}) const;
    void addGuide(bool horizontal, double position);
    void clearGuides();
    bool brushTool() const;
    QImage selectionForLayer(const Layer &layer) const;
    QColor sampleColor(QPointF position, int sampleSize = 1);
  signals:
    void editStarted();
    void editFinished(const QString &label);
    void editCanceled();
    void edited();
    void colorPicked(QColor color);
    void filesDropped(const QStringList &paths);
    void selectionChanged();
    void aiPointPicked(QPointF point, bool exclude);
    void aiApplyRequested();
    void aiCancelRequested();
    void aiRemovePointRequested();
    void selectionEdited(const QString &label, const QImage &before, const QImage &after);
    // The pointer moved over the canvas, to this document point.
    void pointerMoved(QPointF point);
    void sessionChanged();
    void cropRequested(QRect bounds);
    void error(const QString &message);

  protected:
    bool event(QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void leaveEvent(QEvent *) override;
    void hideEvent(QHideEvent *) override;

  private:
    Document *document_;
    CanvasTextEditor *textEditor_ = nullptr;
    QString textLayerID_;
    Layer textBefore_, textFrameBefore_;
    bool textNew_ = false;
    QString textLayerAt(QPointF point) const;
    void updateTextEditing(bool force = false);
    void syncTextEditor();
    EditorSession fallbackSession_;
    EditorSession *session_;
    std::unique_ptr<CanvasTool> controller_;
    Tool controllerKind_ = Tool::Move;
    bool temporaryPan_ = false;
    QImage original_, coverage_, priorSelection_, blurred_;
    std::unique_ptr<WarpBrush> warp_;
    bool warpChanged_ = false;
    QPointF pan_, start_, last_, cloneSource_, cloneOffset_;
    bool dragging_ = false, cloneReady_ = false;
    Qt::KeyboardModifiers selectionModifiers_{};
    QPainterPath lasso_;
    bool aiPicking_ = false;
    QVector<QPointF> aiPoints_;
    QVector<int> aiLabels_;
    QVector<QPointF> polygonPoints_;
    QPainterPath selectionOutline_;
    qint64 selectionOutlineKey_ = 0;
    // The document as drawn: tiles of it at power-of-two reductions (see drawDocument).
    QCache<quint64, QImage> tiles_{256 * 1024}; // cost in KiB
    // Per tile, what lies under the layer being edited, while it is (see beginLayerEdit).
    QCache<quint64, QImage> backdrops_{128 * 1024};
    QString editedLayer_;
    // What each layer was when the tiles were drawn, so a refresh redraws only what changed.
    struct LayerStamp {
        QByteArray metadata;
        qint64 image = 0, mask = 0;
        QRectF extent;
    };
    QHash<QString, LayerStamp> stamps_;
    QStringList stampOrder_;
    QSize stampSize_;
    QImage composite_;
    // Tiles being rendered off the UI thread from a copy of the document; null when none are.
    // `generation_` changes whenever tiles are dropped, so results from an older copy are not used.
    std::shared_ptr<RenderBatch> batch_;
    QFuture<void> batchFuture_;
    QFutureWatcher<void> batchWatcher_;
    quint64 generation_ = 0;
    QRectF strokeArea_;
    // The edited image's pixels the dabs changed since its render caches were last carried over,
    // and its cacheKey then.
    QRect strokePixels_;
    qint64 carriedKey_ = 0;
    // How far, in document pixels, the edited layer's effects carry a change.
    double strokeReach_ = 0;
    QString renderError_;
    QPointF hover_;
    bool hovered_ = false, temporaryPicker_ = false;
    int panPhysicalKey_ = 0;
    QColor hoverColor_ = Qt::transparent;
    int antsPhase_ = 0, guideDrag_ = -1, transformHandle_ = -1, opacityDigit_ = -1;
    QElapsedTimer opacityClock_;
    // What a transform handle or the Move tool edits: one layer, its mask alone, or several
    // layers (selected ones and the contents of selected folders) moved as one box.
    struct TransformSubject {
        enum class Kind { None, Layer, Mask, Group } kind = Kind::None;
        QStringList ids;
        QJsonObject box; // The transform the handles are drawn on: the layer's, mask's or group's.
    };
    // The gesture in progress: what each edited layer was when it began, the box as the handles
    // have drawn it since, and, while distorting, the box's four corners.
    TransformSubject transformSubject_;
    QHash<QString, Layer> transformOriginals_;
    QJsonObject transformDraft_;
    QVector<QPointF> distortStart_, distortCorners_;
    QRectF moveFrame_;
    bool distorting_ = false, duplicating_ = false;
    QImage cloneSample_;
    QTransform cloneSamplePlacement_;
    Layer paintBefore_;
    bool paintChanged_ = false;
    QRect strokeSelectionBounds_;
    QPointF cloneOffsetBefore_;
    bool cloneReadyBefore_ = false;
    bool cloneStrokeReady_ = false;
    QPointF lastBrushPoint_;
    QString lastBrushLayer_;
    bool lastBrushMask_ = false;
    bool fitted_ = false;
    std::function<void(Document &)> livePreview_;
    std::function<void(const PickEvent &)> pickHandler_;
    bool inputLocked_ = false, picking_ = false;
    struct FloatingSession {
        QString floatingId, sourceId;
    };
    std::optional<FloatingSession> floating_;
    QPainterPath floatingOutline_;
    qint64 floatingOutlineKey_ = 0;
    // Dragging inside a selection: its outline alone, or its pixels with it (a floating layer
    // that merges back on release).
    enum class SelectionDrag { None, Outline, Pixels };
    SelectionDrag selectionDrag_ = SelectionDrag::None;
    QJsonObject dragOrigin_;
    QString dragFloatingId_, dragSourceId_;
    QPoint dragOffset_;
    bool dragDuplicate_ = false;
    bool beginSelectionDrag(QMouseEvent *event);
    void updateSelectionDrag(QMouseEvent *event);
    void finishSelectionDrag(QMouseEvent *event);
    void moveSelectedPixels(QPoint offset);
    // Edits that end a gesture report to the page, unless a floating selection is pending: its
    // gestures all belong to the one undo step that ends with merging it.
    void startEdit();
    void finishEdit(const QString &label);
    void restoreGesture();
    QRectF canvasRect() const;
    QPointF toDocument(QPointF point) const;
    void dab(QPointF point);
    void finishSelection();
    void drawDocument(QPainter &painter, const QRectF &target);
    void finishBatch();
    void reportRenderError(const QString &message);
    void drawPixelGrid(QPainter &painter, const QRectF &target);
    void drawCanvasOverlay(QPainter &painter);
    bool beginOverlayEdit(QMouseEvent *event);
    bool moveOverlayEdit(QMouseEvent *event);
    bool finishOverlayEdit(QMouseEvent *event);
    Layer transformTarget(const Layer &layer) const;
    TransformSubject transformSubject() const;
    QVector<QPointF> handlePoints(const QVector<QPointF> &corners, bool rotation) const;
    void beginTransform(const TransformSubject &subject, int handle, bool distort);
    QJsonObject draftTransform(const QJsonObject &box, int handle, QPointF point,
                               Qt::KeyboardModifiers modifiers, const QStringList &ids) const;
    void applyTransform(const QJsonObject &draft);
    void updateDistort(QPointF point, Qt::KeyboardModifiers modifiers);
    void applyDistortion(double limit);
    void endTransform();
    void drawTransformControls(QPainter &painter);
    // The Move tool's drag: the selected layers, a folder's contents with it, or a mask alone.
    void beginMove(bool duplicate);
    void moveBy(QPointF delta, Qt::KeyboardModifiers modifiers);
    void finishMove();
    void selectLayerAt(QPointF point, bool extend);
    bool handleCanvasKey(QKeyEvent *event);
    void nudge(QPointF delta, bool pixels);
    void invalidate(const QRectF &area);
    void forgetTiles();
    void stampAll();
    void refreshStroke();
    void beginLayerEdit(const QString &id);
    void endLayerEdit();
    CanvasTool &controller();
    void beginPaint(QMouseEvent *event);
    void preparePaintArea(QPointF from, QPointF to);
    void paintSegment(QPointF from, QPointF to);
    void continuePaint(QMouseEvent *event);
    void finishPaint(QMouseEvent *event);
    void drawGesture(QPainter &painter);
    friend class PaintTool;
    friend class SelectionTool;
    friend class ShapeTool;
    friend class MoveTool;
    friend class SelectTool;
    friend class PanTool;
    friend class EyedropperTool;
    friend class WandTool;
    friend class TextTool;
    friend class CropTool;
};
} // namespace compositor
