// SPDX-License-Identifier: MIT
#pragma once
#include "canvas.h"
#include "ai_selection.h"
#include <QPointer>
class QDialog;
#include <QKeySequence>
#include <QMainWindow>
#include <QUndoStack>
#include <functional>
class QTabWidget;
class QTreeWidget;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QCloseEvent;
class QMenu;

namespace compositor {
class EditorPage : public QWidget {
    Q_OBJECT
  public:
    explicit EditorPage(Document document, QWidget *parent = nullptr);
    ~EditorPage() override;
    Document document;
    EditorSession session;
    Canvas *canvas;
    QUndoStack history;
    std::shared_ptr<AiSelectionService> aiSelection = createAiSelectionService();
    QString path;
    bool saving = false;
    quint64 revision = 0;
    QStringList importNotes;
    void edit(const QString &label, const std::function<void(Document &)> &operation);
    void record(const QString &label, const Document &before, const Document &after,
                const QImage &beforeSelection, const QImage &afterSelection,
                const QSet<QString> &beforeLayers = {}, const QSet<QString> &afterLayers = {});
    void changed();
    bool isModified() const {
        return contentState != savedContentState_;
    }
    void markSaved(quint64 state);
    quint64 contentState = 0;
    bool interacting() const {
        return interacting_;
    }
  signals:
    void documentChanged();
    void editWillStart();
    void error(const QString &message);

  private:
    Document beforeInteraction_;
    QImage beforeSelection_;
    QSet<QString> beforeInteractionLayers_;
    quint64 nextContentState_ = 0;
    quint64 savedContentState_ = 0;
    bool interacting_ = false;
};

class EditorWindow : public QMainWindow {
    Q_OBJECT
  public:
    EditorWindow();
    void openPath(const QString &path);

  protected:
    void closeEvent(QCloseEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

  private:
    QTabWidget *tabs_;
    QPointer<QDialog> aiSelectionDialog_;
    // A dialog that leaves the window open: the edit it previews shows on the canvas meanwhile.
    QPointer<QDialog> liveDialog_;
    bool runLiveDialog(QDialog &dialog);
    void setLiveEditingLocked(bool locked);
    QList<QPointer<QWidget>> lockedWidgets_;
    QTreeWidget *layers_;
    QComboBox *blend_;
    QDoubleSpinBox *opacity_, *x_, *y_, *width_, *height_, *angle_, *brushSize_, *hardness_,
        *brushOpacity_;
    QCheckBox *maskTarget_;
    QLabel *limitations_;
    QPushButton *colorButton_;
    bool syncing_ = false;
    Tool tool_ = Tool::Move;
    QDoubleSpinBox *blurRadius_, *tolerance_;
    QCheckBox *contiguous_;
    QPushButton *backgroundButton_;
    EditorPage *page() const;
    void addPage(Document document, const QString &path = {});
    void createDocument();
    void importFiles(const QStringList &paths);
    void openPhotoshop(const QString &path);
    void openRaw(const QString &path, bool asDocument);
    void save(bool saveAs = false);
    void exportImage();
    void refreshPanels();
    void rebuildLayerOrder();
    void updateTransform();
    void setTool(Tool tool);
    void filter(const QString &kind, bool asAdjustment = false, bool editExisting = false);
    void editEffect(const QString &key);
    void editText();
    void editShape();
    void addMask();
    void maskFromSelection();
    void resizeSelectionDialog(bool expand);
    void colorRangeDialog();
    void removeBackground();
    void selectSubject();
    void selectObject();
    void fillSelection(bool erase = false, bool background = false);
    void crop();
    bool canClose(EditorPage *page);
    void showError(const QString &message);
    QAction *action(QMenu *menu, const QString &title, const QKeySequence &shortcut,
                    const std::function<void()> &callback);
    void buildMenus();
    void buildViewMenu(QMenu *menu);
    void applyViewOptions(const CanvasViewOptions &options);
    void buildPanels();
    void buildToolOptions();
    void syncToolOptions();
    void buildExtraToolOptions();
    void syncExtraToolOptions();
    QSet<QString> selectedLayers() const;
    void mergeSelectedLayers();
    void groupSelectedLayers();
    void ungroupSelectedLayer();
    void duplicateSelectedLayers();
    void deleteSelectedLayers();
    void copySelectedLayers();
    bool pasteCopiedLayers();
    void canvasSizeDialog();
    void imageSizeDialog();
    void trimDialog();
    void flipDocument(bool horizontal);
    void applyCrop(QRect bounds);
    QAction *mergeAction_ = nullptr;
    QComboBox *selectionMode_ = nullptr, *gradientType_ = nullptr, *pickerSize_ = nullptr;
    QCheckBox *cloneAligned_ = nullptr, *cloneMerged_ = nullptr, *gradientBackground_ = nullptr,
              *gradientReverse_ = nullptr, *autoSelect_ = nullptr;
    QDoubleSpinBox *cornerRadius_ = nullptr, *lineWidth_ = nullptr, *textSize_ = nullptr,
                   *zoomPercent_ = nullptr;
    QComboBox *textFont_ = nullptr;
};
} // namespace compositor
