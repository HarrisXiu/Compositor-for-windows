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
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QMenu>
#include <QDialog>
#include <QMessageBox>
#include <QPainter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

namespace compositor {
EditorPage *EditorWindow::page() const {
    return qobject_cast<EditorPage *>(tabs_->currentWidget());
}
EditorWindow::EditorWindow() {
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
            p->deleteLater();
        }
    });
    addPage(Document::create({1200, 800}));
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
    connect(a, &QAction::triggered, this, [this, callback] {
        try {
            if (aiSelectionDialog_)
                aiSelectionDialog_->reject();
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
    auto snapshot = p->document;
    const auto contentState = p->contentState;
    p->saving = true;
    statusBar()->showMessage(uiText("Saving project in background…"));
    auto watcher = new QFutureWatcher<QString>(p);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, p, watcher, path, contentState] {
                auto error = watcher->result();
                p->saving = false;
                watcher->deleteLater();
                if (!error.isEmpty()) {
                    showError(error);
                    return;
                }
                p->path = path;
                p->markSaved(contentState);
                refreshPanels();
                statusBar()->showMessage(uiText("Saved " + path), 5000);
            });
    watcher->setFuture(QtConcurrent::run([snapshot, path] {
        try {
            saveProject(snapshot, path);
            return QString();
        } catch (const std::exception &e) {
            return QString::fromUtf8(e.what());
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
    p->canvas->finishTextEditing(true);
    p->canvas->commitFloatingSelection();
    if (p->saving) {
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
void EditorWindow::closeEvent(QCloseEvent *e) {
    if (aiSelectionDialog_)
        aiSelectionDialog_->reject();
    for (int i = 0; i < tabs_->count(); ++i)
        if (!canClose(qobject_cast<EditorPage *>(tabs_->widget(i)))) {
            e->ignore();
            return;
        }
    e->accept();
}
void EditorWindow::showError(const QString &message) {
    QMessageBox::warning(this, "Compositor", message);
}
} // namespace compositor
