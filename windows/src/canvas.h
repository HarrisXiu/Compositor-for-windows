// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include "editor_session.h"
#include "warp_brush.h"
#include <QCache>
#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QPainterPath>
#include <QWidget>
#include <memory>

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
    void selectAll();
    void invertSelection();
    void replaceSelection(const QImage &mask, const QString &label);
    void featherSelection(double radius);
    void cancelInteraction();
    void applyCropFrame();
    void cancelCropFrame();
    QRect selectionBounds() const;
    void setTool(Tool value);
    QImage selectionForLayer(const Layer &layer) const;
  signals:
    void editStarted();
    void editFinished(const QString &label);
    void editCanceled();
    void edited();
    void colorPicked(QColor color);
    void filesDropped(const QStringList &paths);
    void selectionChanged();
    void selectionEdited(const QString &label, const QImage &before, const QImage &after);
    void sessionChanged();
    void cropRequested(QRect bounds);
    void error(const QString &message);

  protected:
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

  private:
    Document *document_;
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
    bool fitted_ = false;
    QRectF canvasRect() const;
    QPointF toDocument(QPointF point) const;
    void dab(QPointF point);
    void finishSelection();
    void drawDocument(QPainter &painter, const QRectF &target);
    void finishBatch();
    void reportRenderError(const QString &message);
    void drawPixelGrid(QPainter &painter, const QRectF &target);
    void invalidate(const QRectF &area);
    void forgetTiles();
    void stampAll();
    void refreshStroke();
    void beginLayerEdit(const QString &id);
    void endLayerEdit();
    CanvasTool &controller();
    void beginPaint(QMouseEvent *event);
    void continuePaint(QMouseEvent *event);
    void finishPaint(QMouseEvent *event);
    void drawGesture(QPainter &painter);
    friend class PaintTool;
    friend class SelectionTool;
    friend class ShapeTool;
    friend class MoveTool;
    friend class PanTool;
    friend class EyedropperTool;
    friend class WandTool;
    friend class TextTool;
    friend class CropTool;
};
} // namespace compositor
