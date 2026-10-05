// SPDX-License-Identifier: MIT
#pragma once
#include "preview_runner.h"
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <memory>

namespace compositor {
class Canvas;
class EditorPage;

// A filter or adjustment's settings as they apply to an image shown at `scale` of its size: the
// radii and distances that measure in image pixels shrink with it.
QJsonObject scaledPreviewSettings(const QString &kind, QJsonObject settings, double scale);

// Shows an edit on the canvas while its dialog is open, without touching the document. A filter
// runs in the background on a copy of the layer reduced to what the screen shows (never more than
// 16 megapixels), so the canvas keeps responding and always shows the latest result that finished;
// an adjustment layer is handed to the canvas, which already renders those on the fly.
class FilterPreview final : public QObject {
    Q_OBJECT
  public:
    FilterPreview(EditorPage *page, const QString &kind, bool asAdjustment, bool editExisting,
                  QObject *parent = nullptr);
    ~FilterPreview() override;
    // Whether this kind of edit can be previewed on the canvas (Camera Raw is previewed in its dialog).
    static bool supported(const QString &kind);
    // The settings to show. A filter's preview follows once it has been computed.
    void update(const QJsonObject &settings);
    // Off: the canvas shows the document as it is.
    void setEnabled(bool enabled);
    bool enabled() const {
        return enabled_;
    }
    // A newer preview is still being computed.
    bool busy() const {
        return runner_.pending() || viewTimer_.isActive();
    }
    // Fraction of the layer's size the preview on the canvas is computed at.
    double scale() const {
        return shownScale_;
    }
    int shownCount() const {
        return shown_;
    }
    // Stops: the canvas goes back to the document.
    void stop();

  signals:
    void busyChanged();
    void shown();
    void failed(const QString &message);

  private:
    struct Coverage;
    EditorPage *page_;
    Canvas *canvas_;
    QString kind_, layerId_, adjustmentId_;
    bool asAdjustment_, editExisting_, enabled_ = true, stopped_ = false;
    QJsonObject settings_;
    bool haveSettings_ = false;
    QImage image_;
    QRectF extent_;
    std::shared_ptr<Coverage> coverage_;
    PreviewRunner runner_;
    QTimer viewTimer_;
    double requestedScale_ = 0, shownScale_ = 0;
    int shown_ = 0;
    double wantedScale() const;
    void schedule();
    void viewChanged();
    void showAdjustment();
};
} // namespace compositor
