// SPDX-License-Identifier: MIT
#include "editor.h"
#include "camera_raw.h"
#include "curve_editor.h"
#include "demo.h"
#include "dither.h"
#include "editable_layers.h"
#include "effects.h"
#include "filters.h"
#include "image_scope.h"
#include "language.h"
#include "photoshop.h"
#include "raw_dialog.h"
#include "render.h"
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QImageWriter>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QUndoCommand>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace compositor {
class CameraPreview final : public QLabel {
  public:
    int mode = 0;
    QList<QLineF> guides;
    std::function<void(QPointF)> sampled;
    std::function<void(QLineF)> guided;

  protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton || mode == 0 ||
            !imageRect().contains(event->position()))
            return;
        start_ = position(event->position());
        drawing_ = mode == 2;
        if (mode != 2 && sampled)
            sampled(start_);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (drawing_) {
            end_ = position(event->position());
            update();
        }
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (drawing_) {
            end_ = position(event->position());
            drawing_ = false;
            if (QLineF(start_, end_).length() > .01 && guided)
                guided(QLineF(start_, end_));
            update();
        }
    }
    void paintEvent(QPaintEvent *event) override {
        QLabel::paintEvent(event);
        if (mode != 2)
            return;
        auto r = imageRect();
        auto point = [&](QPointF p) {
            return r.topLeft() + QPointF(p.x() * r.width(), (1 - p.y()) * r.height());
        };
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(90, 230, 180), 2));
        for (auto guide : guides)
            painter.drawLine(point(guide.p1()), point(guide.p2()));
        if (drawing_)
            painter.drawLine(point(start_), point(end_));
    }

  private:
    QPointF start_, end_;
    bool drawing_ = false;
    QRectF imageRect() const {
        auto size = pixmap().deviceIndependentSize();
        return QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(),
                      size.height());
    }
    QPointF position(QPointF p) const {
        auto r = imageRect();
        return {std::clamp((p.x() - r.x()) / r.width(), 0.0, 1.0),
                1 - std::clamp((p.y() - r.y()) / r.height(), 0.0, 1.0)};
    }
};
class LayerTree final : public QTreeWidget {
  public:
    std::function<void()> moved;

  protected:
    void dropEvent(QDropEvent *event) override {
        QTreeWidget::dropEvent(event);
        if (event->isAccepted() && moved)
            QTimer::singleShot(0, this, moved);
    }
};
class DocumentCommand final : public QUndoCommand {
  public:
    DocumentCommand(EditorPage *page, QString label, Document before, Document after)
        : QUndoCommand(label), page_(page), before_(std::move(before)), after_(std::move(after)) {}
    void undo() override {
        page_->document = before_;
        page_->changed();
    }
    void redo() override {
        page_->document = after_;
        page_->changed();
    }

  private:
    EditorPage *page_;
    Document before_, after_;
};
EditorPage::EditorPage(Document source, QWidget *parent)
    : QWidget(parent), document(std::move(source)), history(this) {
    canvas = new Canvas(&document, this);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(canvas);
    history.setUndoLimit(40);
    connect(canvas, &Canvas::editStarted, this, [this] {
        beforeInteraction_ = document;
        interacting_ = true;
    });
    connect(canvas, &Canvas::editFinished, this, [this](const QString &label) {
        interacting_ = false;
        record(label, beforeInteraction_, document);
    });
    connect(canvas, &Canvas::editCanceled, this, [this] {
        if (interacting_) {
            document = beforeInteraction_;
            interacting_ = false;
            changed();
        }
    });
    connect(canvas, &Canvas::error, this, [this](const QString &message) {
        if (interacting_) {
            document = beforeInteraction_;
            interacting_ = false;
            changed();
        }
        emit error(message);
    });
    connect(&history, &QUndoStack::cleanChanged, this, [this] { emit documentChanged(); });
}
void EditorPage::changed() {
    ++revision;
    canvas->refresh();
    emit documentChanged();
}
void EditorPage::record(const QString &label, const Document &before, const Document &after) {
    try {
        after.validateAssets();
        history.push(new DocumentCommand(this, label, before, after));
    } catch (const std::exception &e) {
        document = before;
        changed();
        emit error(QString::fromUtf8(e.what()));
    }
}
void EditorPage::edit(const QString &label, const std::function<void(Document &)> &operation) {
    auto before = document;
    try {
        operation(document);
        record(label, before, document);
    } catch (const std::exception &e) {
        document = before;
        changed();
        emit error(QString::fromUtf8(e.what()));
    }
}
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
    setCentralWidget(tabs_);
    buildMenus();
    buildPanels();
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { refreshPanels(); });
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
    a->setShortcut(shortcut);
    connect(a, &QAction::triggered, this, [this, callback] {
        try {
            callback();
        } catch (const std::exception &e) {
            showError(QString::fromUtf8(e.what()));
        }
    });
    return a;
}
void EditorWindow::buildMenus() {
    auto file = menuBar()->addMenu("&File");
    action(file, "New…", QKeySequence::New, [this] { createDocument(); });
    action(file, "Open Project…", QKeySequence::Open, [this] {
        auto path = QFileDialog::getExistingDirectory(this, "Open .comp Project", {},
                                                      QFileDialog::ShowDirsOnly);
        if (!path.isEmpty())
            openPath(path);
    });
    action(file, "Import Images…", QKeySequence("Ctrl+I"), [this] {
        importFiles(QFileDialog::getOpenFileNames(this, "Import Images", {},
                                                  "Images (*.png *.jpg *.jpeg *.bmp *.webp *.tif "
                                                  "*.tiff *.svg *.psd *.psb);;All Files (*)"));
    });
    action(file, "Develop RAW Photo…", {}, [this] {
        auto path = QFileDialog::getOpenFileName(
            this, "Develop RAW Photo", {}, "Camera RAW (" + rawFilePatterns() + ");;All Files (*)");
        if (!path.isEmpty())
            openRaw(path, false);
    });
    action(file, "Open Photoshop PSD / PSB…", {}, [this] {
        auto path = QFileDialog::getOpenFileName(this, "Open Photoshop File", {},
                                                 "Photoshop (*.psd *.psb);;All Files (*)");
        if (!path.isEmpty())
            openPath(path);
    });
    file->addSeparator();
    action(file, "Save Project", QKeySequence::Save, [this] { save(); });
    action(file, "Save Project As…", QKeySequence::SaveAs, [this] { save(true); });
    action(file, "Export PNG / JPEG…", QKeySequence("Ctrl+Shift+Alt+S"), [this] { exportImage(); });
    file->addSeparator();
    action(file, "Close Project", QKeySequence::Close, [this] {
        int index = tabs_->currentIndex();
        if (index >= 0 && canClose(page())) {
            auto p = page();
            tabs_->removeTab(index);
            p->deleteLater();
        }
    });
    action(file, "Exit", QKeySequence("Alt+F4"), [this] { close(); });
    auto edit = menuBar()->addMenu("&Edit");
    action(edit, "Undo", QKeySequence::Undo, [this] {
        if (page())
            page()->history.undo();
    });
    action(edit, "Redo", QKeySequence::Redo, [this] {
        if (page())
            page()->history.redo();
    });
    edit->addSeparator();
    action(edit, "Copy Merged", QKeySequence("Ctrl+Shift+C"), [this] {
        if (page()) {
            require(page()->document.previewLimitations().isEmpty(),
                    "This project contains effects not yet rendered in this preview");
            QApplication::clipboard()->setImage(renderDocument(page()->document));
        }
    });
    action(edit, "Paste Image", QKeySequence::Paste, [this] {
        auto image = QApplication::clipboard()->image();
        require(!image.isNull(), "Clipboard has no image");
        if (!page())
            addPage(Document::create(image.size()));
        page()->edit("Paste Image", [&](Document &d) { d.addImage("Pasted Image", image); });
    });
    action(edit, "Fill Selection", QKeySequence("Shift+F5"), [this] { fillSelection(); });
    action(edit, "Clear Pixels", QKeySequence(Qt::Key_Delete), [this] { fillSelection(true); });
    auto layer = menuBar()->addMenu("&Layer");
    action(layer, "New Paint Layer", QKeySequence("Ctrl+Shift+N"), [this] {
        if (page())
            page()->edit("New Layer", [](Document &d) { d.addBlank("Layer"); });
    });
    action(layer, "New Folder", {}, [this] {
        if (page())
            page()->edit("New Folder", [](Document &d) { d.addGroup("Folder"); });
    });
    action(layer, "Duplicate Layer", QKeySequence("Ctrl+J"), [this] {
        if (page())
            page()->edit("Duplicate Layer", [](Document &d) { d.duplicate(d.activeId()); });
    });
    action(layer, "Delete Layer", {}, [this] {
        if (page())
            page()->edit("Delete Layer", [](Document &d) { d.remove(d.activeId()); });
    });
    action(layer, "Edit Text…", {}, [this] { editText(); });
    action(layer, "Edit Shape…", {}, [this] { editShape(); });
    auto adjustments = layer->addMenu("New Adjustment Layer");
    for (const auto &kind : QStringList{
             "Hue/Saturation", "Levels", "Curves", "Exposure", "Gradient Map", "Grain", "Invert",
             "Black & White", "Color Balance", "Gaussian Blur", "Motion Blur", "Add Noise"})
        action(adjustments, kind + "…", {}, [this, kind] { filter(kind, true); });
    action(layer, "Edit Adjustment…", {}, [this] {
        if (page() && page()->document.active()) {
            auto a = page()->document.active()->metadata.value("adjustment").toObject();
            require(!a.isEmpty(), "Select an adjustment layer");
            filter(a.value("kind").toString(), true, true);
        }
    });
    auto effects = layer->addMenu("Layer Effects");
    const QStringList effectKeys{"stroke",      "shadow",    "colorOverlay",
                                 "innerShadow", "outerGlow", "innerGlow"},
        effectNames{"Stroke",       "Drop Shadow", "Color Overlay",
                    "Inner Shadow", "Outer Glow",  "Inner Glow"};
    for (int i = 0; i < effectKeys.size(); ++i)
        action(effects, effectNames[i] + "…", {}, [this, key = effectKeys[i]] { editEffect(key); });
    action(effects, "Remove All Effects", {}, [this] {
        if (page())
            page()->edit("Remove Effects", [](Document &d) {
                if (auto l = d.active())
                    l->metadata.remove("effects");
            });
    });
    action(layer, "Create Merged Layer", QKeySequence("Ctrl+Shift+E"), [this] {
        if (page())
            page()->edit("Create Merged Layer", [](Document &d) {
                require(d.previewLimitations().isEmpty(),
                        "Unsupported effects must be migrated before merging");
                auto image = renderDocument(d);
                for (auto &l : d.layers)
                    if (l.parent().isEmpty())
                        l.metadata["isVisible"] = false;
                d.addImage("Merged", image);
            });
    });
    layer->addSeparator();
    action(layer, "Add White Mask", {}, [this] { addMask(); });
    action(layer, "Invert Mask", {}, [this] {
        if (page())
            page()->edit("Invert Mask", [](Document &d) {
                auto l = d.active();
                require(l && !l->mask.isNull(), "Select a layer with a mask");
                l->mask.detach();
                for (int y = 0; y < l->mask.height(); ++y) {
                    auto p = l->mask.scanLine(y);
                    for (int x = 0; x < l->mask.width(); ++x)
                        p[x] = 255 - p[x];
                }
            });
    });
    action(layer, "Toggle Mask", {}, [this] {
        if (page())
            page()->edit("Toggle Mask", [](Document &d) {
                auto l = d.active();
                require(l && !l->mask.isNull(), "Select a mask");
                l->metadata["maskEnabled"] = !l->metadata.value("maskEnabled").toBool(true);
            });
    });
    action(layer, "Link / Unlink Mask", {}, [this] {
        if (page())
            page()->edit("Mask Link", [](Document &d) {
                auto l = d.active();
                require(l && !l->mask.isNull(), "Select a layer with a mask");
                bool linked = l->metadata.value("maskLinked").toBool(true);
                if (linked && !l->metadata.contains("maskPlacement"))
                    l->metadata["maskPlacement"] = l->transform();
                l->metadata["maskLinked"] = !linked;
            });
    });
    action(layer, "Remove Mask", {}, [this] {
        if (page())
            page()->edit("Remove Mask", [](Document &d) {
                if (auto l = d.active()) {
                    l->mask = {};
                    for (auto key : {"maskFile", "maskLinked", "maskEnabled", "maskPlacement"})
                        l->metadata.remove(QLatin1String(key));
                }
            });
    });
    action(layer, "Create / Release Clipping Mask", QKeySequence("Ctrl+Alt+G"), [this] {
        if (page())
            page()->edit("Clipping Mask", [](Document &d) {
                auto l = d.active();
                require(l && !l->group(), "Select an image layer");
                if (l->metadata.contains("maskSourceID")) {
                    l->metadata.remove("maskSourceID");
                    return;
                }
                int index = int(l - d.layers.data());
                for (int i = index - 1; i >= 0; --i)
                    if (!d.layers[i].group() && d.layers[i].parent() == l->parent()) {
                        l->metadata["maskSourceID"] = d.layers[i].id();
                        d.metadata["version"] = CurrentVersion;
                        return;
                    }
                throw Error("No lower sibling layer to clip to");
            });
    });
    action(layer, "Flip Horizontal", {}, [this] {
        if (page())
            page()->edit("Flip Layer", [](Document &d) {
                if (auto l = d.active()) {
                    auto t = l->transform();
                    t["flipX"] = !t.value("flipX").toBool();
                    l->metadata["transform"] = t;
                }
            });
    });
    action(layer, "Flip Vertical", {}, [this] {
        if (page())
            page()->edit("Flip Layer", [](Document &d) {
                if (auto l = d.active()) {
                    auto t = l->transform();
                    t["flipY"] = !t.value("flipY").toBool();
                    l->metadata["transform"] = t;
                }
            });
    });
    auto select = menuBar()->addMenu("&Select");
    action(select, "All", QKeySequence::SelectAll, [this] {
        if (page())
            page()->canvas->selectAll();
    });
    action(select, "Deselect", QKeySequence("Ctrl+D"), [this] {
        if (page())
            page()->canvas->clearSelection();
    });
    action(select, "Inverse", QKeySequence("Ctrl+Shift+I"), [this] {
        if (page())
            page()->canvas->invertSelection();
    });
    action(select, "Feather…", {}, [this] {
        if (!page() || page()->canvas->selection.isNull())
            return;
        bool ok;
        double radius = QInputDialog::getDouble(this, "Feather", "Radius", 2, 0.1, 50, 1, &ok);
        if (ok) {
            auto rgba = page()->canvas->selection.convertToFormat(QImage::Format_RGBA8888);
            auto blurred = gaussianBlur(rgba, radius);
            page()->canvas->selection = blurred.convertToFormat(QImage::Format_Grayscale8);
            page()->canvas->update();
        }
    });
    auto image = menuBar()->addMenu("&Image");
    action(image, "Crop to Selection", {}, [this] { crop(); });
    action(image, "Canvas Size…", {}, [this] {
        if (!page())
            return;
        bool ok;
        int w = QInputDialog::getInt(this, "Canvas Size", "Width", page()->document.size().width(),
                                     1, MaxSide, 1, &ok);
        if (!ok)
            return;
        int h = QInputDialog::getInt(this, "Canvas Size", "Height",
                                     page()->document.size().height(), 1, MaxSide, 1, &ok);
        if (ok) {
            page()->edit("Canvas Size", [&](Document &d) {
                d.metadata["width"] = w;
                d.metadata["height"] = h;
            });
            page()->canvas->clearSelection();
            page()->canvas->fit();
        }
    });
    for (const auto &kind : QStringList{"Invert", "Exposure", "Levels", "Curves", "Hue/Saturation",
                                        "Black & White", "Gradient Map", "Color Balance"})
        action(image, kind + "…", {}, [this, kind] { filter(kind); });
    auto filters = menuBar()->addMenu("&Filter");
    for (const auto &kind : QStringList{"Gaussian Blur", "Motion Blur", "Add Noise", "Grain",
                                        "Lens Correction", "Camera Raw", "Dither", "Vignette",
                                        "Bloom / Glow", "Tonal Contrast", "Content-Aware Fill"})
        action(filters, kind + "…", {}, [this, kind] { filter(kind); });
    auto view = menuBar()->addMenu("&View");
    auto language = view->addMenu("Language");
    language->setObjectName("languageMenu");
    auto languageGroup = new QActionGroup(this);
    const QStringList codes{"zh_CN", "en", "ja_JP"}, names{"简体中文", "English", "日本語"};
    for (int i = 0; i < codes.size(); ++i) {
        auto a = language->addAction(names[i]);
        a->setObjectName("language_" + codes[i]);
        a->setData(codes[i]);
        a->setCheckable(true);
        a->setChecked(UiLanguage::instance().code() == codes[i]);
        languageGroup->addAction(a);
        connect(a, &QAction::triggered, this,
                [code = codes[i]] { UiLanguage::instance().setLanguage(code); });
    }
    connect(&UiLanguage::instance(), &UiLanguage::languageChanged, this, [this, languageGroup] {
        for (auto a : languageGroup->actions())
            a->setChecked(a->data().toString() == UiLanguage::instance().code());
        refreshPanels();
        statusBar()->showMessage(
            uiText("C/C++ Windows preview — Ctrl+O opens .comp folders; Ctrl+I imports images"));
    });
    action(view, "Fit Canvas", QKeySequence("Ctrl+0"), [this] {
        if (page())
            page()->canvas->fit();
    });
    action(view, "Actual Pixels", QKeySequence("Ctrl+1"), [this] {
        if (page()) {
            page()->canvas->zoom = 1;
            page()->canvas->update();
        }
    });
    auto help = menuBar()->addMenu("&Help");
    action(help, "Import Conversion Report…", {}, [this] {
        if (page())
            QMessageBox::information(this, "Import Conversion Report",
                                     page()->importNotes.isEmpty()
                                         ? "No conversions were required for this import."
                                         : page()->importNotes.join("\n\n"));
    });
    action(help, "Open Demo", {}, [this] { addPage(createDemoDocument()); });
    action(help, "About This Preview", {}, [this] {
        QMessageBox::information(
            this, "Compositor Windows",
            "Compositor Windows 0.3\nC++20 / Qt 6 with the original C pixel algorithms.\n\n"
            "PSD/PSB import, RAW development, Dither, Smudge, Liquify, and additional photo "
            "controls are available. GPU rendering, advanced canvas interactions, and AI "
            "segmentation "
            "still require migration. Unsupported metadata is retained "
            "in .comp projects; exports are blocked when the preview cannot faithfully represent a "
            "project.\n\nOriginal Compositor: robbietilton/Compositor (MIT). Qt is dynamically "
            "linked under LGPLv3; LibRaw uses CDDL 1.0. See THIRD_PARTY_NOTICES.md.\nAutomatic "
            "updates are excluded from the Windows port.");
    });
}
void EditorWindow::buildPanels() {
    auto tools = addToolBar("Tools");
    tools->setMovable(false);
    auto group = new QActionGroup(this);
    group->setExclusive(true);
    const struct {
        const char *name;
        Tool tool;
        const char *key;
    } options[] = {{"Move", Tool::Move, "V"},
                   {"Brush", Tool::Brush, "B"},
                   {"Erase", Tool::Erase, "E"},
                   {"Marquee", Tool::RectangleSelect, "M"},
                   {"Ellipse Select", Tool::EllipseSelect, ""},
                   {"Lasso", Tool::Lasso, "L"},
                   {"Wand", Tool::Wand, "W"},
                   {"Gradient", Tool::Gradient, "G"},
                   {"Rectangle", Tool::Rectangle, "U"},
                   {"Ellipse", Tool::Ellipse, ""},
                   {"Line", Tool::Line, ""},
                   {"Text", Tool::Text, "T"},
                   {"Picker", Tool::Eyedropper, "I"},
                   {"Clone", Tool::Clone, "S"},
                   {"Heal", Tool::Heal, "J"},
                   {"Blur", Tool::Blur, "R"},
                   {"Smudge", Tool::Smudge, ""},
                   {"Liquify", Tool::Liquify, ""},
                   {"Pan", Tool::Pan, "H"}};
    for (const auto &option : options) {
        auto a = tools->addAction(option.name);
        a->setCheckable(true);
        a->setShortcut(QKeySequence(option.key));
        group->addAction(a);
        if (option.tool == Tool::Move)
            a->setChecked(true);
        connect(a, &QAction::triggered, this, [this, t = option.tool] { setTool(t); });
    }
    auto brush = addToolBar("Brush");
    brush->setMovable(false);
    addToolBarBreak();
    brush->addWidget(new QLabel("  Size  "));
    brushSize_ = new QDoubleSpinBox;
    brushSize_->setRange(1, 2000);
    brushSize_->setValue(32);
    brush->addWidget(brushSize_);
    brush->addWidget(new QLabel("  Hardness  "));
    hardness_ = new QDoubleSpinBox;
    hardness_->setRange(0, 100);
    hardness_->setValue(80);
    hardness_->setSuffix(" %");
    brush->addWidget(hardness_);
    brush->addWidget(new QLabel("  Opacity  "));
    brushOpacity_ = new QDoubleSpinBox;
    brushOpacity_->setRange(0, 100);
    brushOpacity_->setValue(100);
    brushOpacity_->setSuffix(" %");
    brush->addWidget(brushOpacity_);
    auto blurRadius = new QDoubleSpinBox;
    blurRadius->setRange(.5, 50);
    blurRadius->setValue(5);
    blurRadius->setToolTip("Blur brush radius in canvas pixels");
    brush->addWidget(new QLabel("  Blur radius  "));
    brush->addWidget(blurRadius);
    connect(blurRadius, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (page())
            page()->canvas->blurRadius = v;
    });
    colorButton_ = new QPushButton("Color");
    brush->addWidget(colorButton_);
    connect(colorButton_, &QPushButton::clicked, this, [this] {
        auto c =
            QColorDialog::getColor(color_, this, "Paint Color", QColorDialog::ShowAlphaChannel);
        if (c.isValid()) {
            color_ = c;
            if (page())
                page()->canvas->color = c;
            colorButton_->setStyleSheet("background:" + c.name() + ";color:white;");
        }
    });
    maskTarget_ = new QCheckBox("Paint mask");
    brush->addWidget(maskTarget_);
    auto syncBrush = [this] {
        if (page()) {
            auto c = page()->canvas;
            c->brushSize = brushSize_->value();
            c->hardness = hardness_->value() / 100;
            c->brushOpacity = brushOpacity_->value() / 100;
            c->paintMask = maskTarget_->isChecked();
        }
    };
    connect(brushSize_, &QDoubleSpinBox::valueChanged, this, syncBrush);
    connect(hardness_, &QDoubleSpinBox::valueChanged, this, syncBrush);
    connect(brushOpacity_, &QDoubleSpinBox::valueChanged, this, syncBrush);
    connect(maskTarget_, &QCheckBox::toggled, this, syncBrush);
    auto selectionTools = addToolBar("Selection Options");
    selectionTools->setMovable(false);
    selectionTools->addWidget(new QLabel("Wand tolerance  "));
    auto tolerance = new QDoubleSpinBox;
    tolerance->setRange(0, 255);
    tolerance->setValue(32);
    selectionTools->addWidget(tolerance);
    auto contiguous = new QCheckBox("Contiguous");
    contiguous->setChecked(true);
    selectionTools->addWidget(contiguous);
    connect(tolerance, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (page())
            page()->canvas->wandTolerance = v;
    });
    connect(contiguous, &QCheckBox::toggled, this, [this](bool v) {
        if (page())
            page()->canvas->wandContiguous = v;
    });
    auto dock = new QDockWidget("Layers / Transform", this);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto contents = new QWidget;
    auto layout = new QVBoxLayout(contents);
    limitations_ = new QLabel;
    limitations_->setWordWrap(true);
    limitations_->setStyleSheet("color:#eab676;padding:6px;");
    layout->addWidget(limitations_);
    blend_ = new QComboBox;
    blend_->addItems(blendModes());
    layout->addWidget(blend_);
    opacity_ = new QDoubleSpinBox;
    opacity_->setRange(0, 100);
    opacity_->setSuffix(" % opacity");
    layout->addWidget(opacity_);
    auto layerTree = new LayerTree;
    layerTree->moved = [this] { rebuildLayerOrder(); };
    layers_ = layerTree;
    layers_->setHeaderHidden(true);
    layers_->setDragDropMode(QAbstractItemView::InternalMove);
    layers_->setDefaultDropAction(Qt::MoveAction);
    layers_->setSelectionMode(QAbstractItemView::SingleSelection);
    layers_->setMinimumWidth(255);
    layout->addWidget(layers_, 1);
    auto buttons = new QHBoxLayout;
    for (auto text : {"+ Layer", "+ Folder", "+ Mask"}) {
        auto button = new QPushButton(text);
        buttons->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, label = QString(text)] {
            if (!page())
                return;
            if (label == "+ Mask")
                addMask();
            else
                page()->edit(label, [&](Document &d) {
                    if (label == "+ Layer")
                        d.addBlank("Layer");
                    else
                        d.addGroup("Folder");
                });
        });
    }
    layout->addLayout(buttons);
    auto form = new QFormLayout;
    auto spin = [&](const QString &label, double min, double max) {
        auto s = new QDoubleSpinBox;
        s->setRange(min, max);
        s->setDecimals(2);
        form->addRow(label, s);
        connect(s, &QDoubleSpinBox::editingFinished, this, &EditorWindow::updateTransform);
        return s;
    };
    x_ = spin("X", -1000000, 1000000);
    y_ = spin("Y", -1000000, 1000000);
    width_ = spin("Width", 1, 300000);
    height_ = spin("Height", 1, 300000);
    angle_ = spin("Rotation", -36000, 36000);
    layout->addLayout(form);
    dock->setWidget(contents);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    connect(blend_, &QComboBox::currentTextChanged, this, [this](const QString &) {
        auto value = comboValue(blend_);
        if (!syncing_ && page())
            page()->edit("Blend Mode", [&](Document &d) {
                if (auto l = d.active()) {
                    require(!l->group(), "Folders use Normal blending");
                    l->metadata["blendMode"] = value;
                    d.metadata["version"] = CurrentVersion;
                }
            });
    });
    connect(opacity_, &QDoubleSpinBox::editingFinished, this, [this] {
        if (!syncing_ && page())
            page()->edit("Layer Opacity", [&](Document &d) {
                if (auto l = d.active()) {
                    l->metadata["opacity"] = opacity_->value() / 100;
                    d.metadata["version"] = CurrentVersion;
                }
            });
    });
    connect(layers_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (syncing_ || !page() || !item)
            return;
        page()->document.metadata["activeLayerID"] = item->data(0, Qt::UserRole).toString();
        refreshPanels();
    });
    connect(layers_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (syncing_ || !page())
            return;
        auto id = item->data(0, Qt::UserRole).toString(), name = item->text(0);
        bool visible = item->checkState(0) == Qt::Checked;
        page()->edit("Layer Properties", [&](Document &d) {
            if (auto l = d.find(id)) {
                l->metadata["name"] = name;
                l->metadata["isVisible"] = visible;
            }
        });
    });
}
void EditorWindow::addPage(Document d, const QString &path) {
    auto p = new EditorPage(std::move(d));
    p->path = path;
    int index = tabs_->addTab(p, path.isEmpty() ? uiText("Untitled") : QFileInfo(path).fileName());
    tabs_->setCurrentIndex(index);
    connect(p, &EditorPage::documentChanged, this, &EditorWindow::refreshPanels);
    connect(p, &EditorPage::error, this, &EditorWindow::showError);
    connect(p->canvas, &Canvas::colorPicked, this, [this](QColor c) {
        color_ = c;
        if (page())
            page()->canvas->color = c;
        colorButton_->setStyleSheet("background:" + c.name() + ";color:white;");
    });
    connect(p->canvas, &Canvas::filesDropped, this, &EditorWindow::importFiles);
    setTool(tool_);
    QTimer::singleShot(0, p->canvas, &Canvas::fit);
    refreshPanels();
}
void EditorWindow::createDocument() {
    QDialog dialog(this);
    dialog.setWindowTitle("New Canvas");
    auto layout = new QFormLayout(&dialog);
    QDoubleSpinBox w, h;
    w.setRange(1, MaxSide);
    h.setRange(1, MaxSide);
    w.setValue(1200);
    h.setValue(800);
    w.setDecimals(0);
    h.setDecimals(0);
    layout->addRow("Width", &w);
    layout->addRow("Height", &h);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        addPage(Document::create({int(w.value()), int(h.value())}));
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
    auto snapshot = p->document;
    const auto revision = p->revision;
    p->saving = true;
    statusBar()->showMessage(uiText("Saving project in background…"));
    auto watcher = new QFutureWatcher<QString>(p);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, p, watcher, path, revision] {
        auto error = watcher->result();
        p->saving = false;
        watcher->deleteLater();
        if (!error.isEmpty()) {
            showError(error);
            return;
        }
        p->path = path;
        if (p->revision == revision)
            p->history.setClean();
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
void EditorWindow::exportImage() {
    auto p = page();
    if (!p)
        return;
    require(p->document.previewLimitations().isEmpty(),
            "Export is unavailable because this project contains features not yet rendered: " +
                p->document.previewLimitations().join(", "));
    auto path = QFileDialog::getSaveFileName(this, "Export Image", "Export.png",
                                             "PNG Image (*.png);;JPEG Image (*.jpg)");
    if (path.isEmpty())
        return;
    auto image = renderDocument(p->document);
    image.setDotsPerMeterX(int(p->document.metadata.value("resolution").toDouble(72) / 0.0254));
    image.setDotsPerMeterY(image.dotsPerMeterX());
    QByteArray format = "PNG";
    int quality = 95;
    if (path.endsWith(".jpg", Qt::CaseInsensitive) || path.endsWith(".jpeg", Qt::CaseInsensitive)) {
        format = "JPEG";
        bool ok;
        quality = QInputDialog::getInt(this, "JPEG Quality", "Quality", 95, 1, 100, 1, &ok);
        if (!ok)
            return;
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter painter(&flat);
        painter.drawImage(0, 0, image);
        painter.end();
        flat.setDotsPerMeterX(image.dotsPerMeterX());
        flat.setDotsPerMeterY(image.dotsPerMeterY());
        image = flat;
    }
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot open export destination");
    QImageWriter writer(&file, format);
    writer.setQuality(quality);
    require(writer.write(image) && file.commit(), "Cannot encode exported image");
    statusBar()->showMessage(uiText("Exported " + path), 5000);
}
void EditorWindow::refreshPanels() {
    if (syncing_)
        return;
    syncing_ = true;
    layers_->clear();
    auto p = page();
    for (int i = 0; i < tabs_->count(); ++i) {
        auto tab = qobject_cast<EditorPage *>(tabs_->widget(i));
        if (tab)
            tabs_->setTabText(
                i, (tab->path.isEmpty() ? uiText("Untitled") : QFileInfo(tab->path).fileName()) +
                       (tab->history.isClean() ? "" : " *"));
    }
    if (!p) {
        limitations_->setText("Create or open a project");
        syncing_ = false;
        return;
    }
    auto unsupported = p->document.previewLimitations();
    limitations_->setText(unsupported.isEmpty()
                              ? ""
                              : "Preview pending: " + unsupported.join(", ") +
                                    ". Metadata is preserved; export is blocked.");
    std::function<void(const QString &, QTreeWidgetItem *)> add = [&](const QString &parent,
                                                                      QTreeWidgetItem *item) {
        for (auto it = p->document.layers.crbegin(); it != p->document.layers.crend(); ++it) {
            if (it->parent() != parent)
                continue;
            auto next = item ? new QTreeWidgetItem(item) : new QTreeWidgetItem(layers_);
            next->setText(0, it->name());
            next->setData(0, Qt::UserRole, it->id());
            next->setFlags(next->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable |
                           Qt::ItemIsDragEnabled);
            if (!it->group())
                next->setFlags(next->flags() & ~Qt::ItemIsDropEnabled);
            next->setCheckState(0, it->visible() ? Qt::Checked : Qt::Unchecked);
            if (!it->image.isNull())
                next->setIcon(0, QIcon(QPixmap::fromImage(it->image.scaled(
                                     40, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
            if (it->group()) {
                add(it->id(), next);
                next->setExpanded(true);
            }
            if (it->id() == p->document.activeId())
                layers_->setCurrentItem(next);
        }
    };
    add({}, nullptr);
    if (auto l = p->document.active()) {
        selectComboValue(blend_, l->blend());
        blend_->setEnabled(!l->group());
        opacity_->setValue(l->opacity() * 100);
        auto t = l->transform();
        auto o = t.value("origin").toArray(), s = t.value("size").toArray();
        x_->setValue(o[0].toDouble());
        y_->setValue(o[1].toDouble());
        width_->setValue(s[0].toDouble());
        height_->setValue(s[1].toDouble());
        angle_->setValue(t.value("rotation").toDouble());
        maskTarget_->setEnabled(!l->mask.isNull());
    } else {
        blend_->setEnabled(false);
        maskTarget_->setEnabled(false);
    }
    p->canvas->color = color_;
    p->canvas->brushSize = brushSize_->value();
    p->canvas->hardness = hardness_->value() / 100;
    p->canvas->brushOpacity = brushOpacity_->value() / 100;
    p->canvas->paintMask = maskTarget_->isChecked();
    syncing_ = false;
}
void EditorWindow::rebuildLayerOrder() {
    if (syncing_ || !page())
        return;
    QVector<QPair<QString, QString>> order;
    std::function<void(QTreeWidgetItem *, const QString &)> walk = [&](QTreeWidgetItem *node,
                                                                       const QString &parent) {
        int count = node ? node->childCount() : layers_->topLevelItemCount();
        for (int i = count - 1; i >= 0; --i) {
            auto item = node ? node->child(i) : layers_->topLevelItem(i);
            auto id = item->data(0, Qt::UserRole).toString();
            order.push_back({id, parent});
            walk(item, id);
        }
    };
    walk(nullptr, {});
    page()->edit("Reorder Layers", [&](Document &d) {
        QVector<Layer> next;
        for (const auto &entry : order) {
            auto l = d.find(entry.first);
            require(l, "Missing layer in tree");
            auto copy = *l;
            if (entry.second.isEmpty())
                copy.metadata.remove("parentID");
            else
                copy.metadata["parentID"] = entry.second;
            next.push_back(copy);
        }
        d.layers = next;
        d.metadata["version"] = CurrentVersion;
    });
}
void EditorWindow::updateTransform() {
    if (syncing_ || !page())
        return;
    const double x = x_->value(), y = y_->value(), w = width_->value(), h = height_->value(),
                 a = angle_->value();
    page()->edit("Transform Layer", [&](Document &d) {
        if (auto l = d.active()) {
            l->setBounds({x, y, w, h});
            auto t = l->transform();
            t["rotation"] = a;
            l->metadata["transform"] = t;
        }
    });
}
void EditorWindow::setTool(Tool tool) {
    tool_ = tool;
    if (page())
        page()->canvas->setTool(tool);
}
void EditorWindow::filter(const QString &kind, bool asAdjustment, bool editExisting) {
    auto p = page();
    if (!p)
        return;
    auto layer = p->document.active();
    require(asAdjustment || (layer && !layer->image.isNull()), "Select a pixel layer");
    QJsonObject settings = editExisting
                               ? adjustmentSettings(layer->metadata.value("adjustment").toObject())
                               : QJsonObject();
    auto previewImage =
        asAdjustment
            ? renderDocument(p->document, p->document.size().scaled(256, 256, Qt::KeepAspectRatio))
            : layer->image;
    if (kind != "Invert" && kind != "Content-Aware Fill") {
        QDialog dialog(this);
        dialog.setWindowTitle(kind);
        auto layout = new QVBoxLayout(&dialog);
        auto form = new QFormLayout;
        QStackedWidget *cameraPages = nullptr;
        QComboBox *cameraSection = nullptr;
        if (kind == "Camera Raw") {
            delete form;
            form = nullptr;
            cameraSection = new QComboBox;
            cameraSection->setObjectName("cameraSection");
            cameraPages = new QStackedWidget;
            cameraPages->setMinimumSize(440, 360);
            cameraPages->setMaximumHeight(420);
            layout->addWidget(cameraSection);
            layout->addWidget(cameraPages);
            connect(cameraSection, &QComboBox::currentIndexChanged, cameraPages,
                    &QStackedWidget::setCurrentIndex);
        } else if (kind == "Dither") {
            auto scroll = new QScrollArea;
            auto panel = new QWidget;
            panel->setLayout(form);
            scroll->setWidget(panel);
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setMinimumSize(420, 360);
            scroll->setMaximumHeight(420);
            layout->addWidget(scroll);
        } else
            layout->addLayout(form);
        auto preview = new CameraPreview;
        preview->setObjectName("filterPreview");
        preview->setAlignment(Qt::AlignCenter);
        preview->setMinimumSize(256, 160);
        ImageScope *scope = nullptr;
        if (kind == "Camera Raw") {
            scope = new ImageScope;
            auto row = new QHBoxLayout;
            row->addWidget(preview, 1);
            row->addWidget(scope, 1);
            layout->addLayout(row);
        } else
            layout->addWidget(preview);
        auto source = previewImage.scaled(256, kind == "Camera Raw" ? 160 : 256,
                                          Qt::KeepAspectRatio, Qt::SmoothTransformation);
        auto updatePreview = [&] {
            try {
                if (asAdjustment) {
                    auto snapshot = p->document;
                    auto a = makeAdjustment(kind, settings);
                    if (editExisting)
                        snapshot.active()->metadata["adjustment"] = a;
                    else {
                        Layer l;
                        auto id = newId();
                        l.metadata = {
                            {"id", id},
                            {"name", kind},
                            {"isVisible", true},
                            {"transform", makeTransform(QRectF(QPointF(), snapshot.size()))},
                            {"adjustment", a}};
                        if (!p->canvas->selection.isNull()) {
                            l.mask = p->canvas->selection;
                            l.metadata["maskFile"] = id + ".mask.png";
                        }
                        snapshot.layers.push_back(l);
                    }
                    preview->setPixmap(QPixmap::fromImage(renderDocument(
                        snapshot, snapshot.size().scaled(256, 256, Qt::KeepAspectRatio))));
                    return;
                }
                auto previewSettings = settings;
                double previewScale = double(source.width()) / previewImage.width();
                if (kind == "Camera Raw")
                    previewSettings["previewScale"] = std::min(1.0, previewScale);
                if (kind == "Camera Raw" && preview->mode == 2) {
                    for (auto key :
                         {"geometryVertical", "geometryHorizontal", "geometryRotate",
                          "geometryAspect", "geometryScale", "geometryOffsetX", "geometryOffsetY"})
                        previewSettings[key] = 0;
                    previewSettings["geometryUpright"] = false;
                }
                if (kind == "Bloom / Glow")
                    previewSettings["bloomRadius"] =
                        std::max(.1, settings.value("bloomRadius").toDouble(24) * previewScale);
                if (kind == "Tonal Contrast")
                    previewSettings["tonalRadius"] =
                        std::max(.1, settings.value("tonalRadius").toDouble(16) * previewScale);
                if (kind == "Gaussian Blur")
                    previewSettings["radius"] =
                        settings.value("radius").toDouble(2) * source.width() /
                        (asAdjustment ? p->document.size().width() : layer->image.width());
                if (kind == "Motion Blur")
                    previewSettings["distance"] =
                        std::max(1.0, settings.value("distance").toDouble(10) * source.width() /
                                          (asAdjustment ? p->document.size().width()
                                                        : layer->image.width()));
                QImage result;
                if (scope) {
                    auto gradeSettings = previewSettings;
                    gradeSettings["visualizePointColor"] = -1;
                    auto grade = applyFilter(source, kind, gradeSettings);
                    scope->setImage(grade);
                    result = previewSettings.value("visualizePointColor").toInt(-1) >= 0
                                 ? applyFilter(source, kind, previewSettings)
                                 : grade;
                    if (previewSettings.value("previewSharpenMask").toBool())
                        result = source;
                    cameraRawPreviewOverlay(result, previewSettings);
                } else
                    result = applyFilter(source, kind, previewSettings);
                preview->setPixmap(QPixmap::fromImage(result));
            } catch (const std::exception &e) {
                preview->setText(QString::fromUtf8(e.what()));
            }
        };
        QTimer debounce;
        debounce.setSingleShot(true);
        connect(&debounce, &QTimer::timeout, &dialog, updatePreview);
        QHash<QString, QDoubleSpinBox *> controls;
        QComboBox *rangeControl = nullptr;
        std::function<void()> storeRange = [] {};
        if (kind == "Hue/Saturation" || kind == "Levels") {
            rangeControl = new QComboBox;
            rangeControl->addItems(kind == "Levels"
                                       ? QStringList{"RGB", "Red", "Green", "Blue"}
                                       : QStringList{"Master", "Reds", "Yellows", "Greens", "Cyans",
                                                     "Blues", "Magentas"});
            form->addRow("Range / channel", rangeControl);
            if (kind == "Hue/Saturation") {
                if (!settings.contains("adjustments"))
                    settings["adjustments"] = QJsonObject{
                        {"Master", QJsonObject{{"hue", settings.value("hue")},
                                               {"saturation", settings.value("saturation")},
                                               {"lightness", settings.value("lightness")}}}};
                selectComboValue(rangeControl, settings.value("range").toString("Master"));
                auto current = settings.value("adjustments")
                                   .toObject()
                                   .value(comboValue(rangeControl))
                                   .toObject();
                for (auto key : {"hue", "saturation", "lightness"})
                    settings[QLatin1String(key)] = current.value(QLatin1String(key)).toDouble();
                storeRange = [&, rangeControl] {
                    auto a = settings.value("adjustments").toObject();
                    a[comboValue(rangeControl)] =
                        QJsonObject{{"hue", settings.value("hue")},
                                    {"saturation", settings.value("saturation")},
                                    {"lightness", settings.value("lightness")}};
                    settings["adjustments"] = a;
                    settings["range"] = comboValue(rangeControl);
                };
            } else {
                QJsonObject identity{{"black", 0},
                                     {"white", 255},
                                     {"gamma", 1},
                                     {"outputBlack", 0},
                                     {"outputWhite", 255}};
                if (!settings.contains("ranges"))
                    settings["ranges"] = QJsonArray{identity, identity, identity, identity};
                selectComboValue(rangeControl, settings.value("channel").toString("RGB"));
                auto r =
                    settings.value("ranges").toArray()[rangeControl->currentIndex()].toObject();
                settings["inputBlack"] = r.value("black");
                settings["inputWhite"] = r.value("white");
                for (auto key : {"gamma", "outputBlack", "outputWhite"})
                    settings[QLatin1String(key)] = r.value(QLatin1String(key));
                storeRange = [&, rangeControl] {
                    auto a = settings.value("ranges").toArray();
                    a[rangeControl->currentIndex()] =
                        QJsonObject{{"black", settings.value("inputBlack")},
                                    {"white", settings.value("inputWhite")},
                                    {"gamma", settings.value("gamma")},
                                    {"outputBlack", settings.value("outputBlack")},
                                    {"outputWhite", settings.value("outputWhite")}};
                    settings["ranges"] = a;
                    settings["channel"] = comboValue(rangeControl);
                };
            }
        }
        auto value = [&](const QString &key, const QString &label, double initial, double low,
                         double high, int decimals = 1) {
            auto spin = new QDoubleSpinBox;
            spin->setObjectName(key + "Control");
            spin->setRange(low, high);
            spin->setDecimals(decimals);
            initial = settings.value(key).toDouble(initial);
            spin->setValue(initial);
            controls[key] = spin;
            form->addRow(label, spin);
            settings[key] = initial;
            connect(spin, &QDoubleSpinBox::valueChanged, &dialog, [&, key](double v) {
                settings[key] = v;
                storeRange();
                debounce.start(100);
            });
        };
        auto check = [&](const char *key, const QString &label, bool initial) {
            auto box = new QCheckBox(label);
            box->setObjectName(QString::fromLatin1(key) + "Control");
            initial = settings.value(QLatin1String(key)).toBool(initial);
            box->setChecked(initial);
            form->addRow(box);
            settings[QLatin1String(key)] = initial;
            connect(box, &QCheckBox::toggled, &dialog, [&, key](bool v) {
                settings[QLatin1String(key)] = v;
                debounce.start(100);
            });
        };
        if (kind == "Dither") {
            auto combo = [&](const char *key, const QString &label, const QStringList &items) {
                auto c = new QComboBox;
                c->setObjectName(QString::fromLatin1(key) + "Control");
                c->addItems(items);
                selectComboValue(c, settings.value(QLatin1String(key)).toString(items.first()));
                settings[QLatin1String(key)] = comboValue(c);
                form->addRow(label, c);
                connect(c, &QComboBox::currentTextChanged, &dialog, [&, key, c](const QString &) {
                    settings[QLatin1String(key)] = comboValue(c);
                    debounce.start(100);
                });
                return c;
            };
            combo("style", "Style", ditherStyles());
            combo("colors", "Colors", {"Black & White", "Two Colors", "Original"});
            value("pixelSize", "Pixel size", 2, 1, 32, 0);
            combo("pixelShape", "Pixel shape", {"Square", "Dot"});
            value("levels", "Tones", 2, 2, 8, 0);
            value("diffusion", "Diffusion (%)", 100, 0, 100);
            value("density", "Density", 0, -100, 100);
            value("contrast", "Contrast", 0, -100, 100);
            value("cellSize", "Cell size", 8, 4, 64, 0);
            value("angle", "Screen angle", 45, -90, 90);
            check("lightOnDark", "Light marks on dark", true);
            value("textSize", "Text size", 14, 6, 64, 0);
            auto characters = new QLineEdit(" .:-=+*#%@");
            characters->setObjectName("charactersControl");
            characters->setMaxLength(128);
            settings["characters"] = characters->text();
            form->addRow("Characters", characters);
            connect(characters, &QLineEdit::textChanged, &dialog, [&](const QString &v) {
                settings["characters"] = v;
                debounce.start(100);
            });
            value("lineSpacing", "Line spacing", 4, 2, 32, 0);
            value("glow", "Glow (%)", 35, 0, 100);
            value("dots", "Scanline dots (%)", 0, 0, 100);
            value("wobble", "Wobble", 0, 0, 64);
            for (auto key : {"dark", "light"}) {
                auto button = new QPushButton(QString::fromLatin1(key));
                form->addRow("Two-color palette", button);
                connect(button, &QPushButton::clicked, &dialog, [&, key] {
                    auto obj = settings.value(QLatin1String(key)).toObject();
                    double v = QString::fromLatin1(key) == "light" ? 1 : 0;
                    auto picked =
                        QColorDialog::getColor(QColor::fromRgbF(obj.value("red").toDouble(v),
                                                                obj.value("green").toDouble(v),
                                                                obj.value("blue").toDouble(v)),
                                               &dialog);
                    if (picked.isValid()) {
                        settings[QLatin1String(key)] = QJsonObject{{"red", picked.redF()},
                                                                   {"green", picked.greenF()},
                                                                   {"blue", picked.blueF()}};
                        debounce.start(100);
                    }
                });
            }
        } else if (kind == "Exposure") {
            value("exposure", "Exposure (stops)", 0, -20, 20, 2);
            value("offset", "Offset", 0, -0.5, 0.5, 3);
            value("gamma", "Gamma", 1, 0.01, 9.99, 2);
        } else if (kind == "Camera Raw") {
            auto section = [&](const QString &title) {
                auto scroll = new QScrollArea;
                scroll->setWidgetResizable(true);
                scroll->setFrameShape(QFrame::NoFrame);
                auto panel = new QWidget;
                form = new QFormLayout(panel);
                scroll->setWidget(panel);
                cameraPages->addWidget(scroll);
                cameraSection->addItem(title);
            };
            auto enumeration = [&](const char *key, const QString &label,
                                   const QStringList &items) {
                auto c = new QComboBox;
                c->addItems(items);
                c->setCurrentIndex(settings.value(QLatin1String(key)).toInt());
                form->addRow(label, c);
                settings[QLatin1String(key)] = c->currentIndex();
                connect(c, &QComboBox::currentIndexChanged, &dialog, [&, key](int i) {
                    settings[QLatin1String(key)] = i;
                    debounce.start(100);
                });
            };
            section("Light and color");
            value("temperature", "Temperature", 0, -100, 100);
            value("tint", "Tint", 0, -100, 100);
            auto autoBalance = new QPushButton("Auto white balance");
            autoBalance->setObjectName("autoWhiteBalance");
            form->addRow(autoBalance);
            auto balanceRevision = std::make_shared<quint64>(0);
            for (auto key : {"temperature", "tint"})
                connect(controls[key], &QDoubleSpinBox::valueChanged, &dialog,
                        [balanceRevision](double) { ++*balanceRevision; });
            connect(autoBalance, &QPushButton::clicked, &dialog,
                    [&, autoBalance, balanceRevision, image = previewImage] {
                        autoBalance->setEnabled(false);
                        auto revision = *balanceRevision;
                        auto watcher = new QFutureWatcher<QJsonObject>(&dialog);
                        connect(watcher, &QFutureWatcher<QJsonObject>::finished, &dialog,
                                [&, watcher, autoBalance, balanceRevision, revision] {
                                    auto solved = watcher->result();
                                    watcher->deleteLater();
                                    autoBalance->setEnabled(true);
                                    if (*balanceRevision != revision)
                                        return;
                                    for (auto key : {"temperature", "tint"})
                                        if (solved.contains(key))
                                            controls[key]->setValue(solved.value(key).toDouble());
                                });
                        watcher->setFuture(
                            QtConcurrent::run([image] { return cameraRawWhiteBalance(image); }));
                    });
            value("exposure", "Exposure (stops)", 0, -5, 5, 2);
            for (auto key : {"contrast", "highlights", "shadows", "whites", "blacks", "vibrance",
                             "saturation"})
                value(key, QString::fromLatin1(key), 0, -100, 100);
            section("Curve");
            value("curveShadows", "Shadows", 0, -100, 100);
            value("curveDarks", "Darks", 0, -100, 100);
            value("curveLights", "Lights", 0, -100, 100);
            value("curveHighlights", "Highlights", 0, -100, 100);
            value("curveShadowSplit", "Shadow divider", 25, 5, 90);
            value("curveDarkSplit", "Middle divider", 50, 7, 95);
            value("curveLightSplit", "Highlight divider", 75, 9, 98);
            value("curveRefineSaturation", "Refine saturation", 0, -100, 100);
            const QStringList curveKeys{"curveRGB", "curveRed", "curveGreen", "curveBlue"};
            QJsonArray linear{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 255}, {"y", 255}}};
            for (auto key : curveKeys)
                settings[key] = linear;
            auto channels = new QComboBox;
            channels->setObjectName("cameraCurveChannel");
            channels->addItems({"RGB", "Red", "Green", "Blue"});
            form->addRow("Point curve channel", channels);
            auto pointCurve = new CurveEditor;
            pointCurve->setObjectName("cameraPointCurve");
            pointCurve->points = linear;
            form->addRow(pointCurve);
            pointCurve->changed = [&, channels, curveKeys](const QJsonArray &points) {
                settings[curveKeys[channels->currentIndex()]] = points;
                debounce.start(100);
            };
            connect(channels, &QComboBox::currentIndexChanged, &dialog,
                    [&, pointCurve, curveKeys](int i) {
                        pointCurve->points = settings.value(curveKeys[i]).toArray();
                        pointCurve->update();
                    });
            auto preset = new QComboBox;
            preset->addItems({"Linear", "Medium Contrast", "Strong Contrast"});
            form->addRow("Curve preset", preset);
            connect(preset, &QComboBox::currentIndexChanged, &dialog,
                    [&, pointCurve, channels, curveKeys, linear](int i) {
                        auto points =
                            i == 0
                                ? linear
                                : QJsonArray{
                                      QJsonObject{{"x", 0}, {"y", 0}},
                                      QJsonObject{{"x", 63.75}, {"y", (i == 1 ? .18 : .10) * 255}},
                                      QJsonObject{{"x", 191.25}, {"y", (i == 1 ? .82 : .90) * 255}},
                                      QJsonObject{{"x", 255}, {"y", 255}}};
                        settings[curveKeys[channels->currentIndex()]] = points;
                        pointCurve->points = points;
                        pointCurve->update();
                        debounce.start(100);
                    });
            section("Color Mixer");
            for (auto family : QStringList{"Reds", "Oranges", "Yellows", "Greens", "Aquas", "Blues",
                                           "Purples", "Magentas"})
                for (auto component : QStringList{"Hue", "Saturation", "Luminance"})
                    value("mixer" + family + component, family + " " + component, 0, -100, 100);
            section("Point Color");
            auto pointList = new QComboBox;
            pointList->setObjectName("pointColorList");
            form->addRow("Selected color", pointList);
            const QStringList pointKeys{"hue",      "saturation",      "luminance",
                                        "hueShift", "saturationShift", "luminanceShift",
                                        "hueRange", "saturationRange", "luminanceRange"};
            value("pointHue", "Picked hue", 0, 0, 360);
            value("pointSaturation", "Picked saturation", 0, 0, 1, 2);
            value("pointLuminance", "Picked luminance", .5, 0, 1, 2);
            value("pointHueShift", "Hue shift", 0, -100, 100);
            value("pointSaturationShift", "Saturation shift", 0, -100, 100);
            value("pointLuminanceShift", "Luminance shift", 0, -100, 100);
            value("pointHueRange", "Hue range", 30, 5, 180);
            value("pointSaturationRange", "Saturation range", .4, .05, 1, 2);
            value("pointLuminanceRange", "Luminance range", .4, .05, 1, 2);
            auto visualize = new QCheckBox("Visualize color range");
            form->addRow(visualize);
            auto controlKey = [](QString key) {
                key[0] = key[0].toUpper();
                return "point" + key;
            };
            auto loadPoint = [&, pointList, pointKeys, controlKey, visualize] {
                auto points = settings.value("pointColors").toArray();
                int i = pointList->currentIndex();
                auto point = i >= 0 && i < points.size() ? points[i].toObject() : QJsonObject();
                for (auto key : pointKeys) {
                    auto field = controlKey(key);
                    QSignalBlocker block(controls[field]);
                    auto v = point.value(key).toDouble(settings.value(field).toDouble());
                    controls[field]->setValue(v);
                    controls[field]->setEnabled(!point.isEmpty());
                    settings[field] = v;
                }
                settings["visualizePointColor"] = visualize->isChecked() ? i : -1;
                debounce.start(100);
            };
            auto rebuildPoints = [&, pointList, loadPoint](int selected) {
                QSignalBlocker blocker(pointList);
                pointList->clear();
                auto points = settings.value("pointColors").toArray();
                for (int i = 0; i < points.size(); ++i)
                    pointList->addItem("Color " + QString::number(i + 1));
                pointList->setCurrentIndex(std::min(selected, int(points.size()) - 1));
                UiLanguage::instance().translateObject(pointList);
                loadPoint();
            };
            storeRange = [&, pointList, pointKeys, controlKey] {
                int i = pointList->currentIndex();
                auto points = settings.value("pointColors").toArray();
                if (i < 0 || i >= points.size())
                    return;
                auto point = points[i].toObject();
                for (auto key : pointKeys)
                    point[key] = settings.value(controlKey(key));
                points[i] = point;
                settings["pointColors"] = points;
            };
            auto assignColor = [&, pointList, rebuildPoints](const QColor &c, bool add) {
                auto points = settings.value("pointColors").toArray();
                int i = add ? -1 : pointList->currentIndex();
                if (i < 0) {
                    if (points.size() >= 8)
                        return;
                    i = int(points.size());
                    points.append(QJsonObject{{"hueRange", 30},
                                              {"saturationRange", .4},
                                              {"luminanceRange", .4},
                                              {"hueShift", 0},
                                              {"saturationShift", 0},
                                              {"luminanceShift", 0}});
                }
                auto point = points[i].toObject();
                point["hue"] = std::max(0.0, double(c.hslHueF()) * 360);
                point["saturation"] = c.hslSaturationF();
                point["luminance"] = c.lightnessF();
                points[i] = point;
                settings["pointColors"] = points;
                rebuildPoints(i);
            };
            auto addColor = new QPushButton("Add color…");
            addColor->setObjectName("addPointColor");
            form->addRow(addColor);
            connect(addColor, &QPushButton::clicked, &dialog, [&, assignColor] {
                auto c = QColorDialog::getColor(Qt::red, &dialog, uiText("Point Color"));
                if (c.isValid())
                    assignColor(c, true);
            });
            auto removeColor = new QPushButton("Remove color");
            form->addRow(removeColor);
            connect(removeColor, &QPushButton::clicked, &dialog, [&, pointList, rebuildPoints] {
                int i = pointList->currentIndex();
                auto points = settings.value("pointColors").toArray();
                if (i >= 0 && i < points.size())
                    points.removeAt(i);
                settings["pointColors"] = points;
                rebuildPoints(std::max(0, i - 1));
            });
            connect(pointList, &QComboBox::currentIndexChanged, &dialog,
                    [loadPoint](int) { loadPoint(); });
            connect(visualize, &QCheckBox::toggled, &dialog, [loadPoint](bool) { loadPoint(); });
            rebuildPoints(0);
            auto previewTool = new QComboBox;
            previewTool->setObjectName("cameraPreviewTool");
            previewTool->addItems({"Preview", "Sample point color", "Draw geometry guides",
                                   "Sample white balance", "Sample defringe"});
            auto previewTools = new QHBoxLayout;
            previewTools->addWidget(new QLabel("Preview tool"));
            previewTools->addWidget(previewTool);
            layout->insertLayout(layout->count() - 1, previewTools);
            preview->setToolTip("Choose Sample point color and click the preview; choose Draw "
                                "geometry guides and drag up to four lines.");
            connect(previewTool, &QComboBox::currentIndexChanged, &dialog, [&, preview](int mode) {
                preview->mode = mode;
                preview->setCursor(mode ? Qt::CrossCursor : Qt::ArrowCursor);
                debounce.start(100);
                preview->update();
            });
            preview->sampled = [&, assignColor](QPointF p) {
                int x = std::clamp(int(p.x() * previewImage.width()), 0, previewImage.width() - 1),
                    y = std::clamp(int((1 - p.y()) * previewImage.height()), 0,
                                   previewImage.height() - 1);
                if (preview->mode == 3) {
                    auto solved = cameraRawWhiteBalance(previewImage.copy(x, y, 1, 1));
                    for (auto key : {"temperature", "tint"})
                        if (solved.contains(key))
                            controls[key]->setValue(solved.value(key).toDouble());
                } else if (preview->mode == 4) {
                    double hue =
                        std::max(0.0, double(previewImage.pixelColor(x, y).hsvHueF()) * 360);
                    QString band = std::abs(hue - 290) < std::abs(hue - 90) ? "purple" : "green";
                    controls[band + "HueLow"]->setValue(hue - 25);
                    controls[band + "HueHigh"]->setValue(hue + 25);
                    if (controls[band + "Amount"]->value() == 0)
                        controls[band + "Amount"]->setValue(50);
                } else
                    assignColor(previewImage.pixelColor(x, y), false);
            };
            preview->guided = [&, preview](QLineF line) {
                auto guides = settings.value("geometryGuides").toArray();
                if (guides.size() >= 4)
                    return;
                guides.append(QJsonObject{{"startX", line.x1()},
                                          {"startY", line.y1()},
                                          {"endX", line.x2()},
                                          {"endY", line.y2()}});
                settings["geometryGuides"] = guides;
                settings["geometryUpright"] = true;
                if (auto upright = dialog.findChild<QCheckBox *>("geometryUprightControl"))
                    upright->setChecked(true);
                preview->guides.append(line);
                debounce.start(100);
            };
            section("Color Grading");
            for (auto wheel : QStringList{"Shadows", "Midtones", "Highlights", "Global"}) {
                value("grade" + wheel + "Hue", wheel + " hue", 0, 0, 360);
                value("grade" + wheel + "Saturation", wheel + " saturation", 0, 0, 100);
                value("grade" + wheel + "Luminance", wheel + " luminance", 0, -100, 100);
            }
            value("gradeBlending", "Blending", 50, 0, 100);
            value("gradeBalance", "Balance", 0, -100, 100);
            section("Effects");
            value("texture", "Texture", 0, -100, 100);
            value("clarity", "Clarity", 0, -100, 100);
            value("dehaze", "Dehaze", 0, -100, 100);
            value("glow", "Glow", 0, 0, 100);
            enumeration("glowStyle", "Glow style", {"Diffusion", "Bloom", "Halation"});
            value("glowRange", "Glow range", 0, -100, 100);
            value("glowSpread", "Glow spread", 0, -100, 100);
            value("glowWarmth", "Glow warmth", 0, -100, 100);
            value("vignetteAmount", "Vignette", 0, -100, 100);
            enumeration("vignetteStyle", "Vignette style",
                        {"Highlight Priority", "Color Priority", "Paint Overlay"});
            value("vignetteMidpoint", "Midpoint", 50, 0, 100);
            value("vignetteRoundness", "Roundness", 0, -100, 100);
            value("vignetteFeather", "Feather", 50, 0, 100);
            value("vignetteHighlights", "Protect highlights", 0, 0, 100);
            value("grainAmount", "Grain", 0, 0, 100);
            value("grainSize", "Grain size", 25, 0, 100);
            value("grainRoughness", "Grain roughness", 50, 0, 100);
            section("Detail");
            value("sharpenAmount", "Sharpening", 0, 0, 150);
            value("sharpenRadius", "Sharpening radius", 10, 0, 100);
            value("sharpenDetail", "Sharpening detail", 25, 0, 100);
            value("sharpenMasking", "Masking", 0, 0, 100);
            value("noiseLuminance", "Luminance noise reduction", 0, 0, 100);
            value("noiseLuminanceDetail", "Luminance detail", 50, 0, 100);
            value("noiseLuminanceContrast", "Luminance contrast", 0, 0, 100);
            value("noiseColor", "Color noise reduction", 0, 0, 100);
            value("noiseColorDetail", "Color detail", 50, 0, 100);
            value("noiseColorSmoothness", "Color smoothness", 50, 0, 100);
            section("Optics");
            check("removeChromaticAberration", "Remove chromatic aberration", false);
            check("enableLensProfile", "Generic lens correction", false);
            value("profileDistortion", "Profile distortion strength", 100, 0, 100);
            value("profileVignetting", "Profile vignetting strength", 100, 0, 100);
            value("opticsDistortion", "Manual distortion", 0, -100, 100);
            value("purpleAmount", "Purple defringe", 0, 0, 100);
            value("purpleHueLow", "Purple hue low", 270, 0, 360);
            value("purpleHueHigh", "Purple hue high", 310, 0, 360);
            value("greenAmount", "Green defringe", 0, 0, 100);
            value("greenHueLow", "Green hue low", 60, 0, 360);
            value("greenHueHigh", "Green hue high", 120, 0, 360);
            value("opticsVignetteAmount", "Lens vignette correction", 0, -100, 100);
            value("opticsVignetteMidpoint", "Lens vignette midpoint", 50, 0, 100);
            section("Geometry");
            enumeration("geometryProjection", "Projection", {"Perspective", "Rectilinear"});
            value("geometryVertical", "Vertical", 0, -100, 100);
            value("geometryHorizontal", "Horizontal", 0, -100, 100);
            value("geometryRotate", "Rotate", 0, -45, 45);
            value("geometryAspect", "Aspect", 0, -100, 100);
            value("geometryScale", "Scale", 0, -100, 100);
            value("geometryOffsetX", "Offset X", 0, -100, 100);
            value("geometryOffsetY", "Offset Y", 0, -100, 100);
            check("geometryConstrainCrop", "Constrain crop", false);
            check("geometryUpright", "Guided correction", false);
            auto clearGuides = new QPushButton("Clear geometry guides");
            clearGuides->setObjectName("clearGeometryGuides");
            form->addRow(clearGuides);
            connect(clearGuides, &QPushButton::clicked, &dialog, [&, preview] {
                settings["geometryGuides"] = QJsonArray();
                preview->guides.clear();
                preview->update();
                debounce.start(100);
            });
            section("Calibration");
            value("processVersion", "Process version", 6, 1, 6, 0);
            value("shadowTint", "Shadow tint", 0, -100, 100);
            value("redHue", "Red primary hue", 0, -100, 100);
            value("redSaturation", "Red primary saturation", 0, -100, 100);
            value("greenHue", "Green primary hue", 0, -100, 100);
            value("greenSaturation", "Green primary saturation", 0, -100, 100);
            value("blueHue", "Blue primary hue", 0, -100, 100);
            value("blueSaturation", "Blue primary saturation", 0, -100, 100);
            section("Scope and indicators");
            auto scopeMode = new QComboBox;
            scopeMode->setObjectName("scopeMode");
            scopeMode->addItems({"Histogram", "Vectorscope"});
            form->addRow("Scope", scopeMode);
            connect(scopeMode, &QComboBox::currentIndexChanged, &dialog, [scope](int i) {
                scope->mode = i;
                scope->update();
            });
            check("previewShadowClipping", "Show shadow clipping", false);
            check("previewHighlightClipping", "Show highlight clipping", false);
            check("previewSharpenMask", "Show sharpening mask", false);
        } else if (kind == "Vignette") {
            value("vignetteAmount", "Amount", 35, 0, 100);
            value("vignetteMidpoint", "Midpoint", 50, 0, 100);
            value("vignetteRoundness", "Roundness", 100, -100, 100);
            value("vignetteFeather", "Feather", 60, 0, 100);
            value("vignetteHighlights", "Protect highlights", 25, 0, 100);
            auto button = new QPushButton("Edge color…");
            form->addRow(button);
            connect(button, &QPushButton::clicked, &dialog, [&] {
                auto old = settings.value("vignetteColor").toObject();
                auto c = QColorDialog::getColor(QColor::fromRgbF(old.value("red").toDouble(),
                                                                 old.value("green").toDouble(),
                                                                 old.value("blue").toDouble()),
                                                &dialog);
                if (c.isValid()) {
                    settings["vignetteColor"] =
                        QJsonObject{{"red", c.redF()}, {"green", c.greenF()}, {"blue", c.blueF()}};
                    debounce.start(100);
                }
            });
        } else if (kind == "Bloom / Glow") {
            value("bloomAmount", "Amount", 40, 0, 100);
            value("bloomRadius", "Radius", 24, 1, 150);
        } else if (kind == "Tonal Contrast") {
            value("tonalAmount", "Amount", 50, 0, 100);
            value("tonalRadius", "Detail radius", 16, 1, 100);
            value("tonalShadows", "Shadows", 40, -100, 100);
            value("tonalMidtones", "Midtones", 60, -100, 100);
            value("tonalHighlights", "Highlights", 30, -100, 100);
        } else if (kind == "Gaussian Blur")
            value("radius", "Radius", 2, 0.1, 250);
        else if (kind == "Motion Blur") {
            value("angle", "Angle", 0, -90, 90);
            value("distance", "Distance", 10, 1, 2000);
        } else if (kind == "Add Noise") {
            value("amount", "Amount", 10, 0.1, 400);
            check("gaussian", "Gaussian distribution", false);
            check("monochromatic", "Monochromatic", false);
        } else if (kind == "Grain") {
            value("amount", "Amount", 25, 0, 100);
            value("size", "Size", 1.5, 0.5, 20);
            value("roughness", "Roughness", 50, 0, 100);
        } else if (kind == "Lens Correction")
            value("distortion", "Distortion", 0, -100, 100);
        else if (kind == "Hue/Saturation") {
            value("hue", "Hue", 0, -360, 360);
            value("saturation", "Saturation", 0, -100, 100);
            value("lightness", "Lightness", 0, -100, 100);
            check("colorize", "Colorize", false);
        } else if (kind == "Curves") {
            auto channels = new QComboBox;
            channels->addItems({"RGB", "Red", "Green", "Blue"});
            form->addRow("Channel", channels);
            QJsonArray identity{QJsonObject{{"x", 0}, {"y", 0}},
                                QJsonObject{{"x", 255}, {"y", 255}}};
            if (!settings.contains("channels"))
                settings["channels"] = QJsonArray{identity, identity, identity, identity};
            auto curve = new CurveEditor;
            curve->points = settings.value("channels").toArray()[0].toArray();
            layout->insertWidget(1, curve);
            curve->changed = [&, channels](const QJsonArray &points) {
                auto a = settings.value("channels").toArray();
                a[channels->currentIndex()] = points;
                settings["channels"] = a;
                debounce.start(100);
            };
            connect(channels, &QComboBox::currentIndexChanged, &dialog, [&, curve](int i) {
                curve->points = settings.value("channels").toArray()[i].toArray();
                curve->update();
            });
        } else if (kind == "Levels") {
            value("inputBlack", "Input black", 0, 0, 254, 0);
            value("inputWhite", "Input white", 255, 1, 255, 0);
            value("gamma", "Gamma", 1, 0.1, 9.99, 2);
            value("outputBlack", "Output black", 0, 0, 255, 0);
            value("outputWhite", "Output white", 255, 0, 255, 0);
        } else if (kind == "Black & White") {
            const char *keys[] = {"reds", "yellows", "greens", "cyans", "blues", "magentas"};
            const double defaults[] = {40, 60, 40, 60, 20, 80};
            for (int i = 0; i < 6; ++i)
                value(keys[i], QString::fromLatin1(keys[i]), defaults[i], -200, 300);
            check("tint", "Tint", false);
            value("tintHue", "Tint hue", 40, 0, 360);
            value("tintSaturation", "Tint saturation", 20, 0, 100);
        } else if (kind == "Color Balance") {
            for (auto key : {"shadowCyanRed", "shadowMagentaGreen", "shadowYellowBlue",
                             "midCyanRed", "midMagentaGreen", "midYellowBlue", "highlightCyanRed",
                             "highlightMagentaGreen", "highlightYellowBlue"})
                value(key, QString::fromLatin1(key), 0, -100, 100);
            check("preserveLuminosity", "Preserve luminosity", true);
        } else if (kind == "Gradient Map") {
            if (!settings.contains("shadows"))
                settings["shadows"] = QJsonObject{{"red", 0}, {"green", 0}, {"blue", 0}};
            if (!settings.contains("highlights"))
                settings["highlights"] = QJsonObject{{"red", 1}, {"green", 1}, {"blue", 1}};
            for (auto key : {"shadows", "highlights"}) {
                auto button = new QPushButton(QString::fromLatin1(key));
                form->addRow(button);
                connect(button, &QPushButton::clicked, &dialog, [&, key] {
                    auto old = settings.value(QLatin1String(key)).toObject();
                    auto color = QColorDialog::getColor(
                        QColor::fromRgbF(old.value("red").toDouble(), old.value("green").toDouble(),
                                         old.value("blue").toDouble()),
                        &dialog);
                    if (color.isValid()) {
                        settings[QLatin1String(key)] = QJsonObject{{"red", color.redF()},
                                                                   {"green", color.greenF()},
                                                                   {"blue", color.blueF()}};
                        debounce.start(100);
                    }
                });
            }
            check("reversed", "Reverse", false);
        }
        if (rangeControl) {
            storeRange();
            connect(rangeControl, &QComboBox::currentTextChanged, &dialog,
                    [&, rangeControl](const QString &) {
                        auto range = comboValue(rangeControl);
                        QJsonObject current;
                        if (kind == "Hue/Saturation") {
                            settings["range"] = range;
                            current =
                                settings.value("adjustments").toObject().value(range).toObject();
                        } else {
                            settings["channel"] = range;
                            current = settings.value("ranges")
                                          .toArray()[rangeControl->currentIndex()]
                                          .toObject();
                            current["inputBlack"] = current.value("black");
                            current["inputWhite"] = current.value("white");
                        }
                        for (auto it = controls.begin(); it != controls.end(); ++it) {
                            QSignalBlocker block(it.value());
                            double v = current.value(it.key()).toDouble(
                                it.key() == "gamma"                                     ? 1
                                : it.key() == "inputWhite" || it.key() == "outputWhite" ? 255
                                                                                        : 0);
                            it.value()->setValue(v);
                            settings[it.key()] = v;
                        }
                        debounce.start(100);
                    });
        }
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        updatePreview();
        if (dialog.exec() != QDialog::Accepted)
            return;
    }
    if (kind == "Camera Raw")
        settings["visualizePointColor"] = -1;
    if (asAdjustment) {
        auto a = makeAdjustment(kind, settings);
        p->edit(editExisting ? "Edit Adjustment" : "New Adjustment", [&](Document &d) {
            if (editExisting) {
                auto merged = d.active()->metadata.value("adjustment").toObject();
                for (auto it = a.begin(); it != a.end(); ++it)
                    merged[it.key()] = it.value();
                d.active()->metadata["adjustment"] = merged;
            } else {
                Layer l;
                auto id = newId();
                l.metadata = {{"id", id},
                              {"name", kind},
                              {"isVisible", true},
                              {"opacity", 1},
                              {"blendMode", "Normal"},
                              {"transform", makeTransform(QRectF(QPointF(), d.size()))},
                              {"adjustment", a}};
                if (!p->canvas->selection.isNull()) {
                    l.mask = p->canvas->selection;
                    l.metadata["maskFile"] = id + ".mask.png";
                }
                d.layers.push_back(l);
                d.metadata["activeLayerID"] = id;
            }
            d.metadata["version"] = CurrentVersion;
        });
        return;
    }
    auto coverage = p->canvas->selectionForLayer(*layer);
    if (kind == "Content-Aware Fill") {
        require(!coverage.isNull(), "Select the area to fill first");
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        coverage.save(&buffer, "PNG");
        settings["maskPNG"] = QString::fromLatin1(bytes.toBase64());
    }
    p->edit(kind, [&](Document &d) {
        auto l = d.active();
        auto filtered = applyFilter(l->image, kind, settings);
        l->image = limitToSelection(l->image, filtered, coverage);
        l->metadata.remove("text");
        l->metadata.remove("shape");
    });
}
void EditorWindow::editEffect(const QString &key) {
    auto p = page();
    if (!p)
        return;
    auto layer = p->document.active();
    require(layer && !layer->image.isNull(), "Select an image layer");
    auto all = layer->metadata.value("effects").toObject();
    auto e = all.value(key).toObject();
    bool glow = key == "outerGlow" || key == "innerGlow",
         shadow = key == "shadow" || key == "innerShadow",
         inner = key == "innerShadow" || key == "innerGlow";
    if (e.isEmpty())
        e = {{"enabled", true},
             {"red", glow ? 1 : 0},
             {"green", glow ? 1 : 0},
             {"blue", glow ? 1 : 0},
             {"opacity", glow     ? .75
                         : shadow ? .5
                                  : 1}};
    QDialog dialog(this);
    dialog.setWindowTitle("Layer Effect: " + key);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    layout->addLayout(form);
    auto preview = new QLabel;
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumSize(256, 160);
    layout->addWidget(preview);
    auto source = layer->image.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    double scale = double(source.width()) / layer->image.width();
    QTimer timer;
    timer.setSingleShot(true);
    auto refresh = [&] {
        try {
            auto scaled = e;
            for (auto field : {"size", "blur", "distance"})
                if (scaled.contains(QLatin1String(field)))
                    scaled[QLatin1String(field)] =
                        scaled.value(QLatin1String(field)).toDouble() * scale;
            auto image = renderEffects(source, {{key, scaled}}).image;
            preview->setPixmap(QPixmap::fromImage(
                image.scaled(360, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        } catch (const std::exception &error) {
            preview->setText(QString::fromUtf8(error.what()));
        }
    };
    connect(&timer, &QTimer::timeout, &dialog, refresh);
    auto number = [&](const char *field, const QString &label, double initial, double low,
                      double high) {
        auto spin = new QDoubleSpinBox;
        spin->setRange(low, high);
        spin->setDecimals(2);
        spin->setValue(e.value(QLatin1String(field)).toDouble(initial));
        e[QLatin1String(field)] = spin->value();
        form->addRow(label, spin);
        connect(spin, &QDoubleSpinBox::valueChanged, &dialog, [&, field](double v) {
            e[QLatin1String(field)] = v;
            timer.start(100);
        });
    };
    auto check = [&](const char *field, const QString &label, bool initial) {
        auto box = new QCheckBox(label);
        box->setChecked(e.value(QLatin1String(field)).toBool(initial));
        e[QLatin1String(field)] = box->isChecked();
        form->addRow(box);
        connect(box, &QCheckBox::toggled, &dialog, [&, field](bool v) {
            e[QLatin1String(field)] = v;
            timer.start(100);
        });
    };
    check("enabled", "Enabled", true);
    number("opacity", "Opacity", 1, 0, 1);
    if (key == "stroke") {
        number("size", "Width", 4, 0, 500);
        check("inside", "Inside", false);
    }
    if (glow)
        number("size", "Size", inner ? 10 : 20, 0, 500);
    if (shadow) {
        number("angle", "Light angle", 90, -360, 360);
        number("distance", "Distance", inner ? 10 : 20, 0, 5000);
        number("blur", "Blur", inner ? 10 : 20, 0, 500);
    }
    auto color = new QPushButton("Color…");
    form->addRow(color);
    connect(color, &QPushButton::clicked, &dialog, [&] {
        auto c = QColorDialog::getColor(QColor::fromRgbF(e.value("red").toDouble(),
                                                         e.value("green").toDouble(),
                                                         e.value("blue").toDouble()),
                                        &dialog);
        if (c.isValid()) {
            e["red"] = c.redF();
            e["green"] = c.greenF();
            e["blue"] = c.blueF();
            timer.start(100);
        }
    });
    bool remove = false;
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto removeButton = buttons->addButton("Remove", QDialogButtonBox::DestructiveRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(removeButton, &QPushButton::clicked, &dialog, [&] {
        remove = true;
        dialog.accept();
    });
    refresh();
    if (dialog.exec() != QDialog::Accepted)
        return;
    p->edit(remove ? "Remove Effect" : "Edit Effect", [&](Document &d) {
        auto effects = d.active()->metadata.value("effects").toObject();
        if (remove)
            effects.remove(key);
        else
            effects[key] = e;
        validateEffects(effects);
        if (effects.isEmpty())
            d.active()->metadata.remove("effects");
        else
            d.active()->metadata["effects"] = effects;
    });
}
void EditorWindow::editText() {
    auto p = page();
    if (!p)
        return;
    auto layer = p->document.active();
    require(layer && layer->metadata.value("text").isObject(), "Select an editable text layer");
    auto style = layer->metadata.value("text").toObject();
    QDialog dialog(this);
    dialog.setWindowTitle("Edit Text");
    dialog.resize(640, 700);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    layout->addLayout(form);
    auto text = new QTextEdit;
    loadTextDocument(*text->document(), style);
    layout->addWidget(text, 1);
    auto font = new QFontComboBox;
    font->setCurrentFont(QFont(style.value("fontName").toString()));
    form->addRow("Font (selection or all)", font);
    auto changeAll = [&](const QTextCharFormat &format) {
        auto cursor = text->textCursor();
        if (!cursor.hasSelection())
            cursor.select(QTextCursor::Document);
        cursor.mergeCharFormat(format);
    };
    connect(font, &QFontComboBox::currentFontChanged, &dialog, [&](const QFont &face) {
        auto cursor = text->textCursor();
        if (!cursor.hasSelection())
            style["fontName"] = face.family();
        QTextCharFormat format;
        format.setFontFamilies({face.family()});
        changeAll(format);
    });
    auto size = new QDoubleSpinBox;
    size->setRange(1, 2000);
    size->setValue(style.value("fontSize").toDouble(72));
    form->addRow("Font size (pixels)", size);
    auto tracking = new QDoubleSpinBox;
    tracking->setRange(-100, 1000);
    tracking->setValue(style.value("tracking").toDouble());
    form->addRow("Tracking", tracking);
    auto leading = new QDoubleSpinBox;
    leading->setRange(0, 5000);
    leading->setValue(style.value("leading").toDouble());
    leading->setSpecialValueText("Auto");
    form->addRow("Leading", leading);
    auto alignment = new QComboBox;
    alignment->addItems({"Left", "Center", "Right"});
    selectComboValue(alignment, style.value("alignment").toString("Left"));
    form->addRow("Alignment", alignment);
    auto updateTypography = [&] {
        style["fontSize"] = size->value();
        style["tracking"] = tracking->value();
        style["leading"] = leading->value();
        style["alignment"] = comboValue(alignment);
        auto cursor = text->textCursor();
        cursor.select(QTextCursor::Document);
        QTextCharFormat format;
        format.setProperty(QTextFormat::FontPixelSize, size->value());
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing(tracking->value());
        cursor.mergeCharFormat(format);
        QTextBlockFormat block;
        block.setAlignment(alignment->currentIndex() == 1   ? Qt::AlignHCenter
                           : alignment->currentIndex() == 2 ? Qt::AlignRight
                                                            : Qt::AlignLeft);
        block.setLineHeight(leading->value() > 0 ? leading->value() : size->value() * 1.2,
                            QTextBlockFormat::FixedHeight);
        cursor.mergeBlockFormat(block);
    };
    for (auto spin : {size, tracking, leading})
        connect(spin, &QDoubleSpinBox::valueChanged, &dialog, [&] { updateTypography(); });
    connect(alignment, &QComboBox::currentIndexChanged, &dialog, [&] { updateTypography(); });
    auto color = new QPushButton("Text color (selection or all)…");
    form->addRow(color);
    connect(color, &QPushButton::clicked, &dialog, [&] {
        auto c = QColorDialog::getColor(text->textColor(), &dialog);
        if (c.isValid()) {
            if (!text->textCursor().hasSelection()) {
                style["red"] = c.redF();
                style["green"] = c.greenF();
                style["blue"] = c.blueF();
            }
            QTextCharFormat f;
            f.setForeground(c);
            changeAll(f);
        }
    });
    auto box = style.value("boxSize").toArray();
    auto width = new QDoubleSpinBox, height = new QDoubleSpinBox;
    for (auto spin : {width, height}) {
        spin->setRange(16, MaxSide);
        spin->setDecimals(0);
    }
    width->setValue(box.size() == 2 ? box[0].toDouble() : std::max(16, layer->image.width()));
    height->setValue(box.size() == 2 ? box[1].toDouble() : std::max(16, layer->image.height()));
    form->addRow("Paragraph width", width);
    form->addRow("Paragraph height", height);
    auto paragraph = new QCheckBox("Fixed paragraph box");
    paragraph->setChecked(box.size() == 2);
    form->addRow(paragraph);
    auto updateBox = [&] {
        text->document()->setTextWidth(paragraph->isChecked() ? width->value() : -1);
    };
    connect(paragraph, &QCheckBox::toggled, &dialog, [&] { updateBox(); });
    connect(width, &QDoubleSpinBox::valueChanged, &dialog, [&] { updateBox(); });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    style = textStyleFromDocument(*text->document(), style);
    if (paragraph->isChecked())
        style["boxSize"] = QJsonArray{width->value(), height->value()};
    else
        style.remove("boxSize");
    auto image = renderText(style);
    auto oldSize = layer->image.size();
    p->edit("Edit Text", [&](Document &d) {
        auto l = d.active();
        auto t = l->transform();
        auto s = t.value("size").toArray();
        t["size"] = QJsonArray{s[0].toDouble() * image.width() / oldSize.width(),
                               s[1].toDouble() * image.height() / oldSize.height()};
        l->image = image;
        l->metadata["text"] = style;
        l->metadata["transform"] = t;
        d.metadata["version"] = CurrentVersion;
    });
}
void EditorWindow::editShape() {
    auto p = page();
    if (!p)
        return;
    auto layer = p->document.active();
    require(layer && layer->metadata.value("shape").isObject(), "Select an editable shape layer");
    auto style = layer->metadata.value("shape").toObject();
    QDialog dialog(this);
    dialog.setWindowTitle("Edit Shape");
    auto form = new QFormLayout(&dialog);
    auto kind = new QComboBox;
    kind->addItems({"Rectangle", "Ellipse", "Line"});
    selectComboValue(kind, style.value("kind").toString());
    form->addRow("Shape", kind);
    auto radius = new QDoubleSpinBox;
    radius->setRange(0, MaxSide);
    radius->setValue(style.value("cornerRadius").toDouble());
    form->addRow("Corner radius", radius);
    auto width = new QDoubleSpinBox;
    width->setRange(.1, 5000);
    width->setValue(style.value("lineWidth").toDouble(4));
    form->addRow("Line width", width);
    auto color = new QPushButton("Color…");
    form->addRow(color);
    connect(color, &QPushButton::clicked, &dialog, [&] {
        auto c = QColorDialog::getColor(QColor::fromRgbF(style.value("red").toDouble(),
                                                         style.value("green").toDouble(),
                                                         style.value("blue").toDouble()),
                                        &dialog);
        if (c.isValid()) {
            style["red"] = c.redF();
            style["green"] = c.greenF();
            style["blue"] = c.blueF();
        }
    });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    style["kind"] = comboValue(kind);
    style["cornerRadius"] = radius->value();
    if (comboValue(kind) == "Line") {
        style["lineWidth"] = width->value();
        if (!style.contains("start"))
            style["start"] = QJsonArray{0, 0};
        if (!style.contains("end"))
            style["end"] = QJsonArray{1, 1};
    }
    auto s = layer->transform().value("size").toArray();
    auto image =
        renderShape(style, QSize(int(std::ceil(s[0].toDouble())), int(std::ceil(s[1].toDouble()))));
    p->edit("Edit Shape", [&](Document &d) {
        d.active()->metadata["shape"] = style;
        d.active()->image = image;
    });
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
void EditorWindow::fillSelection(bool erase) {
    auto p = page();
    if (!p)
        return;
    auto l = p->document.active();
    require(l && !l->image.isNull(), "Select a pixel layer");
    auto coverage = p->canvas->selectionForLayer(*l);
    p->edit(erase ? "Clear Pixels" : "Fill Selection", [&](Document &d) {
        auto layer = d.active();
        auto image = layer->image;
        image.detach();
        QPainter painter(&image);
        if (erase)
            painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(image.rect(), erase ? QColor(Qt::transparent) : color_);
        painter.end();
        layer->image = limitToSelection(layer->image, image, coverage);
        layer->metadata.remove("text");
        layer->metadata.remove("shape");
    });
}
void EditorWindow::crop() {
    auto p = page();
    if (!p)
        return;
    auto bounds = p->canvas->selectionBounds().intersected(QRect(QPoint(), p->document.size()));
    require(!bounds.isEmpty(), "Make a selection to crop");
    p->edit("Crop Canvas", [&](Document &d) {
        d.metadata["width"] = bounds.width();
        d.metadata["height"] = bounds.height();
        for (auto &l : d.layers)
            l.move(-QPointF(bounds.topLeft()));
        auto guides = d.metadata.value("guides").toArray();
        for (int i = 0; i < guides.size(); ++i) {
            auto guide = guides[i].toObject();
            guide["position"] = guide.value("position").toDouble() -
                                (guide.value("axis") == "horizontal" ? bounds.y() : bounds.x());
            guides[i] = guide;
        }
        if (d.metadata.contains("guides"))
            d.metadata["guides"] = guides;
    });
    p->canvas->clearSelection();
    p->canvas->fit();
}
bool EditorWindow::canClose(EditorPage *p) {
    if (!p)
        return true;
    if (p->saving) {
        showError("Wait for the background save to finish");
        return false;
    }
    if (p->history.isClean())
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
