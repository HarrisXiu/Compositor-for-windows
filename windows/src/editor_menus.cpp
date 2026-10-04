// SPDX-License-Identifier: MIT
#include "demo.h"
#include "ai_models_dialog.h"
#include "editor.h"
#include "filters.h"
#include "language.h"
#include "layer_operations.h"
#include "raw_import.h"
#include "render.h"
#include "shortcuts.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>

namespace compositor {
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
        if (page()) {
            page()->canvas->finishTextEditing(true);
            page()->canvas->cancelInteraction();
            page()->history.undo();
        }
    });
    action(edit, "Redo", QKeySequence::Redo, [this] {
        if (page()) {
            page()->canvas->finishTextEditing(true);
            page()->canvas->cancelInteraction();
            page()->history.redo();
        }
    });
    edit->addSeparator();
    action(edit, "Keyboard Shortcuts…", QKeySequence("Ctrl+Alt+Shift+K"),
           [this] { Shortcuts::instance().showDialog(this); });
    action(edit, "Copy Layers", QKeySequence::Copy, [this] { copySelectedLayers(); });
    action(edit, "Cut Layers", QKeySequence::Cut, [this] {
        copySelectedLayers();
        deleteSelectedLayers();
    });
    action(edit, "Copy Merged", QKeySequence("Ctrl+Shift+C"), [this] {
        if (page()) {
            require(page()->document.previewLimitations().isEmpty(),
                    "This project contains effects not yet rendered in this preview");
            QApplication::clipboard()->setImage(renderDocument(page()->document));
        }
    });
    action(edit, "Paste Image", QKeySequence::Paste, [this] {
        if (pasteCopiedLayers())
            return;
        auto image = QApplication::clipboard()->image();
        require(!image.isNull(), "Clipboard has no image");
        if (!page())
            addPage(Document::create(image.size()));
        page()->edit("Paste Image", [&](Document &d) { d.addImage("Pasted Image", image); });
    });
    action(edit, "Fill Selection", QKeySequence("Shift+F5"), [this] { fillSelection(); });
    action(edit, "Fill with Foreground", QKeySequence("Alt+Delete"), [this] { fillSelection(); });
    action(edit, "Fill with Background", QKeySequence("Ctrl+Delete"),
           [this] { fillSelection(false, true); });
    action(edit, "Delete Selection / Layer", QKeySequence(Qt::Key_Delete), [this] {
        if (page() && page()->session.selection.isNull() && !page()->canvas->paintMask() &&
            (page()->session.tool == Tool::Move || page()->session.tool == Tool::Select))
            deleteSelectedLayers();
        else
            fillSelection(true);
    });
    action(edit, "Clear Pixels", {}, [this] { fillSelection(true); });
    auto layer = menuBar()->addMenu("&Layer");
    action(layer, "New Paint Layer", QKeySequence("Ctrl+Shift+N"), [this] {
        if (page())
            page()->edit("New Layer", [](Document &d) { d.addBlank("Layer"); });
    });
    action(layer, "New Folder", {}, [this] {
        if (page())
            page()->edit("New Folder", [](Document &d) { d.addGroup("Folder"); });
    });
    action(layer, "Duplicate Layer", QKeySequence("Ctrl+J"), [this] { duplicateSelectedLayers(); });
    action(layer, "Delete Layer", {}, [this] { deleteSelectedLayers(); });
    action(layer, "Group Layers", QKeySequence("Ctrl+G"), [this] { groupSelectedLayers(); });
    action(layer, "Ungroup Layers", QKeySequence("Ctrl+Shift+G"),
           [this] { ungroupSelectedLayer(); });
    mergeAction_ =
        action(layer, "Merge Down", QKeySequence("Ctrl+E"), [this] { mergeSelectedLayers(); });
    action(layer, "Move Layer Up", QKeySequence("Ctrl+]"), [this] {
        if (page())
            page()->edit("Move Layer Up", [&](Document &d) { moveLayers(d, selectedLayers(), 1); });
    });
    action(layer, "Move Layer Down", QKeySequence("Ctrl+["), [this] {
        if (page())
            page()->edit("Move Layer Down",
                         [&](Document &d) { moveLayers(d, selectedLayers(), -1); });
    });
    action(layer, "Move Out of Folder", {}, [this] {
        if (page())
            page()->edit("Move Out of Folder",
                         [&](Document &d) { moveLayersOut(d, selectedLayers()); });
    });
    action(layer, "Edit Text…", {}, [this] { editText(); });
    action(layer, "Edit Shape…", {}, [this] { editShape(); });
    action(layer, "Transform Layer", QKeySequence("Ctrl+T"), [this] {
        setTool(Tool::Move);
        for (auto a : findChildren<QAction *>())
            if (a->property("layerAction").toString() == "Show Transform Controls")
                a->setChecked(true);
    });
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
    action(layer, "Mask from Selection", {}, [this] { maskFromSelection(); });
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
        if (!page() || page()->canvas->session().selection.isNull())
            return;
        bool ok;
        double radius = QInputDialog::getDouble(this, "Feather", "Radius", 2, 0.1, 50, 1, &ok);
        if (ok) {
            page()->canvas->featherSelection(radius);
        }
    });
    action(select, "Expand…", {}, [this] { resizeSelectionDialog(true); });
    action(select, "Contract…", {}, [this] { resizeSelectionDialog(false); });
    action(select, "Color Range…", {}, [this] { colorRangeDialog(); });
    action(select, "Mask from Selection", {}, [this] { maskFromSelection(); });
    auto image = menuBar()->addMenu("&Image");
    action(image, "Crop to Selection", {}, [this] { crop(); });
    action(image, "Canvas Size…", QKeySequence("Ctrl+Alt+C"), [this] { canvasSizeDialog(); });
    action(image, "Image Size…", QKeySequence("Ctrl+Alt+I"), [this] { imageSizeDialog(); });
    action(image, "Trim…", {}, [this] { trimDialog(); });
    action(image, "Flip Canvas Horizontal", {}, [this] { flipDocument(true); });
    action(image, "Flip Canvas Vertical", {}, [this] { flipDocument(false); });
    for (const auto &kind : QStringList{"Invert", "Exposure", "Levels", "Curves", "Hue/Saturation",
                                        "Black & White", "Gradient Map", "Color Balance"})
        action(image, kind + "…",
               QKeySequence(kind == "Curves"           ? "Ctrl+M"
                            : kind == "Levels"         ? "Ctrl+L"
                            : kind == "Hue/Saturation" ? "Ctrl+U"
                            : kind == "Invert"         ? "Ctrl+Alt+Shift+I"
                                                       : ""),
               [this, kind] {
                   if (kind == "Invert" && page() && page()->canvas->paintMask()) {
                       page()->edit("Invert Mask", [](Document &d) {
                           auto l = d.active();
                           require(l && !l->mask.isNull(), "Select a layer with a mask");
                           l->mask.invertPixels();
                       });
                   } else
                       filter(kind);
               });
    auto filters = menuBar()->addMenu("&Filter");
    for (const auto &kind : QStringList{"Gaussian Blur", "Motion Blur", "Add Noise", "Grain",
                                        "Lens Correction", "Camera Raw", "Dither", "Vignette",
                                        "Bloom / Glow", "Tonal Contrast", "Content-Aware Fill"})
        action(filters, kind + "…",
               QKeySequence(kind == "Content-Aware Fill" ? "Shift+Delete" : ""),
               [this, kind] { filter(kind); });
    auto view = menuBar()->addMenu("&View");
    buildViewMenu(view);
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
            page()->canvas->zoomTo(1);
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
    action(help, "AI Models…", {}, [this] {
        AiModelsDialog dialog(this);
        dialog.exec();
    })->setObjectName("aiModelsAction");
    action(help, "Open Demo", {}, [this] { addPage(createDemoDocument()); });
    action(help, "About This Preview", {}, [this] {
        QMessageBox::information(
            this, "Compositor Windows",
            "Compositor Windows 0.4\nC++20 / Qt 6 with the original C pixel algorithms.\n\n"
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
} // namespace compositor
