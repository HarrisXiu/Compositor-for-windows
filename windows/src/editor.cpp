// SPDX-License-Identifier: MIT
#include "editor.h"
#include "filters.h"
#include "language.h"
#include "paint_surface.h"
#include "photoshop.h"
#include "raw_dialog.h"
#include "render.h"
#include "shortcuts.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QMenu>
#include <QDockWidget>
#include <QEventLoop>
#include <QMessageBox>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <algorithm>
#include <QtConcurrent/QtConcurrentRun>

namespace compositor {
EditorPage *EditorWindow::page() const {
    return qobject_cast<EditorPage *>(tabs_->currentWidget());
}
EditorWindow::EditorWindow(std::shared_ptr<RecoveryStore> recovery)
    : recovery_(std::move(recovery)) {
    setWindowTitle("Compositor — Windows Preview");
    resize(1440, 900);
    setMinimumSize(900, 600);
    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->tabBar()->setAcceptDrops(true);
    tabs_->tabBar()->installEventFilter(this);
    setCentralWidget(tabs_);
    buildMenus();
    buildPanels();
    Shortcuts::instance().validateLoaded();
    qApp->installEventFilter(this);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        if (aiSelectionDialog_)
            aiSelectionDialog_->reject();
        refreshPanels();
    });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        auto p = qobject_cast<EditorPage *>(tabs_->widget(index));
        if (p && canClose(p)) {
            tabs_->removeTab(index);
            if (recovery_)
                recovery_->remove(p->recoveryId);
            p->deleteLater();
        }
    });
    addPage(Document::create({1200, 800}));
    if (recovery_ || qApp->applicationName() == "Compositor") {
        try {
            if (!recovery_)
                recovery_ = std::make_shared<RecoveryStore>(
                    QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                    "/recovery");
            connect(&autosaveTimer_, &QTimer::timeout, this, &EditorWindow::autosave);
            int seconds = QSettings().value("files/autosaveSeconds", 60).toInt();
            if (seconds > 0)
                autosaveTimer_.start(std::clamp(seconds, 1, 3600) * 1000);
            QTimer::singleShot(0, this, &EditorWindow::recoverProjects);
        } catch (const std::exception &e) {
            statusBar()->showMessage(QString::fromUtf8(e.what()));
        }
    }
    statusBar()->showMessage(
        uiText("C/C++ Windows preview — Ctrl+O opens .comp folders; Ctrl+I imports images"));
    UiLanguage::instance().translateObject(this, true);
}
QAction *EditorWindow::action(QMenu *menu, const QString &title, const QKeySequence &shortcut,
                              const std::function<void()> &callback) {
    auto a = menu->addAction(title);
    a->setProperty("layerAction", title);
    a->setShortcut(shortcut);
    auto menuSource = menu->menuAction()->property("_uiSource_text").toString();
    if (menuSource.isEmpty())
        menuSource = menu->title();
    Shortcuts::instance().registerAction(a, "menu/" + menuSource + "/" + title, "Menus");
    connect(a, &QAction::triggered, this, [this, callback, a] {
        try {
            // While a filter's result is being made, only the view can change.
            if (applyingFilter_ && !a->property("keepsLiveDialog").toBool())
                return;
            if (aiSelectionDialog_)
                aiSelectionDialog_->reject();
            // Any command but changing the view leaves the dialog and what it previews behind.
            if (liveDialog_ && !a->property("keepsLiveDialog").toBool())
                liveDialog_->reject();
            callback();
        } catch (const std::exception &e) {
            showError(QString::fromUtf8(e.what()));
        }
    });
    return a;
}
void EditorWindow::addPage(Document d, const QString &path) {
    auto p = new EditorPage(std::move(d));
    p->path = path;
    p->monitor->setPath(path);
    connect(
        p->monitor, &ProjectMonitor::projectReady, this,
        [this, p](const Document &d, const QByteArray &digest) { externalReload(p, d, digest); });
    connect(p->monitor, &ProjectMonitor::reloadFailed, this, [this](const QString &error) {
        statusBar()->showMessage(uiText("External project update could not be loaded: ") + error,
                                 10000);
    });
    int index = tabs_->addTab(p, path.isEmpty() ? uiText("Untitled") : QFileInfo(path).fileName());
    tabs_->setCurrentIndex(index);
    connect(p, &EditorPage::documentChanged, this, &EditorWindow::refreshPanels);
    connect(p, &EditorPage::error, this, &EditorWindow::showError);
    connect(p->canvas, &Canvas::colorPicked, this, [this, p](QColor c) {
        p->session.foreground = c;
        if (page() == p)
            syncToolOptions();
    });
    connect(p->canvas, &Canvas::sessionChanged, this, [this, p] {
        if (page() == p)
            refreshPanels();
    });
    connect(p->canvas, &Canvas::filesDropped, this, &EditorWindow::importFiles);
    connect(p->canvas, &Canvas::cropRequested, this, [this, p](QRect bounds) {
        if (page() == p)
            applyCrop(bounds);
    });
    setTool(tool_);
    QTimer::singleShot(0, p->canvas, &Canvas::fit);
    refreshPanels();
}
void EditorWindow::openPath(const QString &path) {
    try {
        const auto absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        for (int i = 0; i < tabs_->count(); ++i) {
            auto p = qobject_cast<EditorPage *>(tabs_->widget(i));
            // Windows paths name the same folder whatever their letter case.
            if (p && !p->path.isEmpty() &&
                QDir::cleanPath(QFileInfo(p->path).absoluteFilePath())
                        .compare(absolute, Qt::CaseInsensitive) == 0) {
                tabs_->setCurrentIndex(i);
                return;
            }
        }
        if (QFileInfo(path).isDir())
            addPage(loadProject(path), path);
        else if (isPhotoshopFile(path))
            openPhotoshop(path);
        else if (isRawFile(path))
            openRaw(path, true);
        else {
            auto image = importImage(path);
            auto d = Document::create(image.size());
            d.addImage(QFileInfo(path).completeBaseName(), image);
            addPage(d);
        }
        if (!isPhotoshopFile(path))
            rememberFile(path);
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void EditorWindow::importFiles(const QStringList &paths) {
    for (const auto &path : paths) {
        if (QFileInfo(path).isDir() || isPhotoshopFile(path)) {
            openPath(path);
            continue;
        }
        try {
            if (isRawFile(path)) {
                openRaw(path, false);
                continue;
            }
            auto image = importImage(path);
            if (!page())
                addPage(Document::create(image.size()));
            page()->edit("Import Image", [&](Document &d) {
                d.addImage(QFileInfo(path).completeBaseName(), image);
            });
        } catch (const std::exception &e) {
            showError(QString::fromUtf8(e.what()));
        }
    }
}
void EditorWindow::openRaw(const QString &path, bool asDocument) {
    RawDevelopDialog dialog(path, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto image = dialog.importedImage();
    if (image.isNull())
        return;
    if (asDocument || !page())
        addPage(Document::create(image.size()));
    page()->edit("Import RAW Photo",
                 [&](Document &d) { d.addImage(QFileInfo(path).completeBaseName(), image); });
}
void EditorWindow::openPhotoshop(const QString &path) {
    statusBar()->showMessage(uiText("Importing Photoshop layers…"));
    using Result = std::pair<PhotoshopImport, QString>;
    auto watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, path] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (!result.second.isEmpty()) {
            showError(result.second);
            return;
        }
        addPage(std::move(result.first.document));
        rememberFile(path);
        page()->importNotes = std::move(result.first.conversions);
        tabs_->setTabText(tabs_->currentIndex(), QFileInfo(path).completeBaseName());
        statusBar()->showMessage(
            uiText(page()->importNotes.isEmpty()
                       ? "Imported Photoshop layers"
                       : "Imported with conversions — Help > Import Conversion Report"),
            10000);
    });
    watcher->setFuture(QtConcurrent::run([path] {
        try {
            return Result{importPhotoshop(path), QString()};
        } catch (const std::exception &e) {
            return Result{PhotoshopImport{}, QString::fromUtf8(e.what())};
        }
    }));
}
void EditorWindow::save(bool saveAs) {
    auto p = page();
    if (!p)
        return;
    require(!p->saving, "A save is already running");
    auto path = p->path;
    if (path.isEmpty() || saveAs) {
        path = QFileDialog::getSaveFileName(this, "Save Project",
                                            path.isEmpty() ? "Untitled.comp" : path,
                                            "Compositor Project (*.comp)");
        if (path.isEmpty())
            return;
        if (!path.endsWith(".comp", Qt::CaseInsensitive))
            path += ".comp";
    }
    p->canvas->finishTextEditing(true);
    p->canvas->commitFloatingSelection();
    p->canvas->cancelInteraction();
    bool replace = path != p->path;
    if (!replace && p->externalConflict) {
        if (QMessageBox::warning(
                this, uiText("Project Changed on Disk"),
                uiText("Saving will replace external changes. Replace the project on disk?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        replace = true;
    }
    startSave(p, path, replace);
}
void EditorWindow::startSave(EditorPage *p, const QString &path, bool replaceChanges) {
    RecoveryStore::cleanStaging(QFileInfo(path).absolutePath());
    auto snapshot = p->document;
    const auto contentState = p->contentState;
    const auto accepted = p->monitor->fingerprint();
    p->saving = true;
    p->monitor->setPaused(true);
    statusBar()->showMessage(uiText("Saving project in background…"));
    struct SaveResult {
        QByteArray fingerprint;
        QString error;
        bool changedOnDisk = false;
    };
    auto watcher = new QFutureWatcher<SaveResult>(p);
    connect(watcher, &QFutureWatcher<SaveResult>::finished, this,
            [this, p, watcher, path, contentState] {
                auto result = watcher->result();
                p->saving = false;
                watcher->deleteLater();
                if (result.changedOnDisk) {
                    p->monitor->setPaused(false);
                    statusBar()->clearMessage();
                    // Includes a project left damaged on disk (a missing image, say): the
                    // document open here can replace it.
                    if (QMessageBox::warning(
                            this, uiText("Project Changed on Disk"),
                            uiText("Saving will replace external changes. Replace the project on disk?"),
                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                        startSave(p, path, true);
                    return;
                }
                if (!result.error.isEmpty()) {
                    p->monitor->setPaused(false);
                    showError(result.error);
                    return;
                }
                p->path = path;
                p->externalConflict = false;
                p->monitor->setPath(path, result.fingerprint);
                p->monitor->setPaused(false);
                rememberFile(path);
                p->markSaved(contentState);
                if (recovery_ && !p->isModified() && !p->autosaving) {
                    recovery_->remove(p->recoveryId);
                    p->recoveredState.reset();
                }
                refreshPanels();
                statusBar()->showMessage(uiText("Saved " + path), 5000);
            });
    // Hashing the project on disk is as slow as reading all of it, so it happens here too.
    watcher->setFuture(QtConcurrent::run([snapshot, path, accepted, replaceChanges] {
        try {
            const auto current = projectFingerprint(path);
            if (!replaceChanges && !accepted.isEmpty() && current != accepted)
                return SaveResult{{}, {}, true};
            return SaveResult{saveProject(snapshot, path, current), {}};
        } catch (const std::exception &e) {
            return SaveResult{{}, QString::fromUtf8(e.what())};
        }
    }));
}
void EditorWindow::setTool(Tool tool) {
    if (aiSelectionDialog_)
        aiSelectionDialog_->reject();
    tool_ = tool;
    if (page())
        page()->canvas->setTool(tool);
    syncToolOptions();
}
void EditorWindow::addMask() {
    if (!page())
        return;
    page()->edit("Add Layer Mask", [](Document &d) {
        auto l = d.active();
        require(l, "Select a layer");
        auto size = l->image.isNull() ? d.size() : l->image.size();
        l->mask = QImage(size, QImage::Format_Grayscale8);
        require(!l->mask.isNull(), "Not enough memory for mask");
        l->mask.fill(255);
        l->metadata["maskFile"] = l->id() + ".mask.png";
        l->metadata["maskEnabled"] = true;
        d.metadata["version"] = CurrentVersion;
    });
}
void EditorWindow::fillSelection(bool erase, bool background) {
    auto p = page();
    if (!p)
        return;
    auto l = p->document.active();
    const bool mask = p->canvas->paintMask();
    require(l && !(mask ? l->mask : l->image).isNull(), "Select a pixel layer or mask");
    const auto area = p->session.selection.isNull() ? QRect(QPoint(), p->document.size())
                                                    : p->canvas->selectionBounds();
    if (area.isEmpty())
        return;
    p->edit(erase ? "Clear Pixels" : "Fill Selection", [&](Document &d) {
        auto layer = d.active();
        if (mask)
            preparePaintMask(*layer);
        auto target = paintTarget(*layer, mask);
        if (!erase) {
            growPaintSurface(*layer, mask, paintSurfaceBounds(target, area));
            target = paintTarget(*layer, mask);
        }
        auto coverage = p->canvas->selectionForLayer(target);
        auto original = (mask ? layer->mask : layer->image)
                            .convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        auto image = original;
        image.detach();
        QPainter painter(&image);
        if (erase)
            painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(image.rect(),
                         erase ? QColor(mask ? Qt::black : Qt::transparent)
                               : (background ? p->session.background : p->session.foreground));
        painter.end();
        auto filled = limitToSelection(original, image, coverage);
        if (mask)
            layer->mask = filled.convertToFormat(QImage::Format_Grayscale8);
        else {
            layer->image = filled;
            layer->metadata.remove("text");
            layer->metadata.remove("shape");
        }
    });
}
void EditorWindow::crop() {
    auto p = page();
    if (!p)
        return;
    auto bounds = p->canvas->selectionBounds().intersected(QRect(QPoint(), p->document.size()));
    require(!bounds.isEmpty(), "Make a selection to crop");
    applyCrop(bounds);
}
bool EditorWindow::canClose(EditorPage *p) {
    if (!p)
        return true;
    if (applyingFilter_) {
        showError("Wait for the filter to finish");
        return false;
    }
    p->canvas->finishTextEditing(true);
    p->canvas->commitFloatingSelection();
    if (p->saving || p->autosaving) {
        showError("Wait for the background save to finish");
        return false;
    }
    if (!p->isModified())
        return true;
    auto result =
        QMessageBox::warning(this, "Unsaved Project", "This project has unsaved changes.",
                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (result == QMessageBox::Discard)
        return true;
    if (result == QMessageBox::Save) {
        tabs_->setCurrentWidget(p);
        save();
    }
    return false;
}
// Shows a dialog without blocking the window, and returns once it has ended. Editing is locked
// meanwhile: the canvas only pans and zooms, and the panels, tool bars and tabs are disabled, so
// the dialog's edit still applies to the layer it was opened on. Commands from the menus end it first.
bool EditorWindow::runLiveDialog(QDialog &dialog, bool sidePanel) {
    require(!liveDialog_, "Another dialog is already open");
    liveDialog_ = &dialog;
    setLiveEditingLocked(true);
    struct Unlock {
        EditorWindow *window;
        ~Unlock() {
            window->setLiveEditingLocked(false);
            window->liveDialog_ = nullptr;
        }
    } unlock{this};
    dialog.setModal(false);
    dialog.adjustSize();
    // Beside the canvas rather than over it; a side panel runs down the window's right edge.
    const auto frame = frameGeometry();
    if (sidePanel) {
        dialog.resize(std::max(dialog.width(), 460), std::max(dialog.height(), frame.height() - 120));
        dialog.move(std::max(frame.left(), frame.right() - dialog.width() - 8), frame.top() + 70);
    } else
        dialog.move(std::max(frame.left(), frame.right() - dialog.width() - 24), frame.top() + 90);
    QEventLoop loop;
    connect(&dialog, &QDialog::finished, &loop, &QEventLoop::quit);
    dialog.show();
    dialog.raise();
    if (dialog.isVisible())
        loop.exec();
    return dialog.result() == QDialog::Accepted;
}
void EditorWindow::setLiveEditingLocked(bool locked) {
    if (auto p = page())
        p->canvas->setInputLocked(locked);
    if (!locked) {
        for (auto widget : lockedWidgets_)
            if (widget)
                widget->setEnabled(true);
        lockedWidgets_.clear();
        return;
    }
    QList<QWidget *> candidates{tabs_->tabBar()};
    for (auto dock : findChildren<QDockWidget *>())
        candidates << dock;
    for (auto bar : findChildren<QToolBar *>())
        candidates << bar;
    for (auto widget : candidates)
        if (widget->isEnabled()) {
            widget->setEnabled(false);
            lockedWidgets_ << widget;
        }
}
void EditorWindow::closeEvent(QCloseEvent *e) {
    if (applyingFilter_) {
        e->ignore();
        return;
    }
    if (liveDialog_)
        liveDialog_->reject();
    if (aiSelectionDialog_)
        aiSelectionDialog_->reject();
    for (int i = 0; i < tabs_->count(); ++i)
        if (!canClose(qobject_cast<EditorPage *>(tabs_->widget(i)))) {
            e->ignore();
            return;
        }
    e->accept();
    if (recovery_)
        for (int i = 0; i < tabs_->count(); ++i)
            if (auto p = qobject_cast<EditorPage *>(tabs_->widget(i)))
                recovery_->remove(p->recoveryId);
}
void EditorWindow::showError(const QString &message) {
    QMessageBox::warning(this, "Compositor", message);
}
} // namespace compositor
