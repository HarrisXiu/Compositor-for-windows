// SPDX-License-Identifier: MIT
#pragma once
#include "canvas.h"
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
    Document document;
    Canvas *canvas;
    QUndoStack history;
    QString path;
    bool saving = false;
    quint64 revision = 0;
    QStringList importNotes;
    void edit(const QString &label, const std::function<void(Document &)> &operation);
    void record(const QString &label, const Document &before, const Document &after);
    void changed();
  signals:
    void documentChanged();
    void error(const QString &message);

  private:
    Document beforeInteraction_;
    bool interacting_ = false;
};

class EditorWindow : public QMainWindow {
    Q_OBJECT
  public:
    EditorWindow();
    void openPath(const QString &path);

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    QTabWidget *tabs_;
    QTreeWidget *layers_;
    QComboBox *blend_;
    QDoubleSpinBox *opacity_, *x_, *y_, *width_, *height_, *angle_, *brushSize_, *hardness_,
        *brushOpacity_;
    QCheckBox *maskTarget_;
    QLabel *limitations_;
    QPushButton *colorButton_;
    bool syncing_ = false;
    Tool tool_ = Tool::Move;
    QColor color_ = QColor(68, 157, 245);
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
    void fillSelection(bool erase = false);
    void crop();
    bool canClose(EditorPage *page);
    void showError(const QString &message);
    QAction *action(QMenu *menu, const QString &title, const QKeySequence &shortcut,
                    const std::function<void()> &callback);
    void buildMenus();
    void buildPanels();
};
} // namespace compositor
