// SPDX-License-Identifier: MIT
#include "editor.h"
#include "adjustment_dialogs.h"
#include "adjustment_panels.h"
#include "camera_raw.h"
#include "curve_editor.h"
#include "demo.h"
#include "dither.h"
#include "editable_layers.h"
#include "effects.h"
#include "filter_preview.h"
#include "filters.h"
#include "image_scope.h"
#include "language.h"
#include "parameter_control.h"
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
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
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
#include <memory>
#include <optional>

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
void EditorWindow::filter(const QString &kind, bool asAdjustment, bool editExisting) {
    auto p = page();
    if (!p)
        return;
    auto layer = p->document.active();
    require(asAdjustment || (layer && !layer->image.isNull()), "Select a pixel layer");
    const auto targetId = p->document.activeId();
    QJsonObject settings = editExisting
                               ? adjustmentSettings(layer->metadata.value("adjustment").toObject())
                               : QJsonObject();
    auto previewImage =
        asAdjustment
            ? renderDocument(p->document, p->document.size().scaled(256, 256, Qt::KeepAspectRatio))
            : layer->image;
    if (kind != "Invert" && kind != "Content-Aware Fill") {
        QDialog dialog(this, Qt::Tool);
        dialog.setObjectName("filterDialog");
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
            // A side panel down the window's edge: the sections take the height there is.
            cameraPages->setMinimumSize(440, 360);
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
            // The previewed color under the pointer on the canvas.
            auto readout = new QLabel("R —   G —   B —");
            readout->setObjectName("cameraRawReadout");
            readout->setToolTip("Red, green, and blue of the pixel under the pointer.");
            layout->addWidget(readout);
        } else
            layout->addWidget(preview);
        auto source = previewImage.scaled(256, kind == "Camera Raw" ? 160 : 256,
                                          Qt::KeepAspectRatio, Qt::SmoothTransformation);
        // The edit shows on the canvas itself while the dialog stays open; the small preview in the
        // dialog remains only for what the canvas cannot show (Camera Raw).
        std::unique_ptr<FilterPreview> live;
        // Levels, Curves and Hue/Saturation's own panels (histograms, handles, eyedroppers).
        std::unique_ptr<AdjustmentDialog> tools;
        if (FilterPreview::supported(kind)) {
            live = std::make_unique<FilterPreview>(p, kind, asAdjustment, editExisting);
            if (kind != "Camera Raw")
                preview->hide();
            if (auto readout = dialog.findChild<QLabel *>("cameraRawReadout")) {
                // Follows the pointer, and the preview under a pointer held still.
                auto pointer = std::make_shared<std::optional<QPointF>>();
                auto show = [&live, readout, pointer] {
                    const auto color = live && *pointer ? live->shownColor(**pointer) : QColor();
                    readout->setText(color.isValid() ? QString("R %1   G %2   B %3")
                                                           .arg(color.red())
                                                           .arg(color.green())
                                                           .arg(color.blue())
                                                     : QString("R —   G —   B —"));
                };
                connect(p->canvas, &Canvas::pointerMoved, readout, [pointer, show](QPointF point) {
                    *pointer = point;
                    show();
                });
                connect(live.get(), &FilterPreview::shown, readout, show);
            }
            connect(live.get(), &FilterPreview::failed, &dialog,
                    [this](const QString &message) { statusBar()->showMessage(uiText(message), 5000); });
        }
        auto updatePreview = [&] {
            if (tools)
                refreshAdjustmentTools(*tools, kind);
            if (live) {
                live->update(settings);
                // Camera Raw keeps its own preview for the scopes and sampling tools.
                if (kind != "Camera Raw")
                    return;
            }
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
                        if (!p->canvas->session().selection.isNull()) {
                            l.mask = p->canvas->session().selection;
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
        QHash<QString, ParameterControl *> parameters;
        CurveEditor *curveWidget = nullptr;
        QComboBox *curveChannels = nullptr;
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
            // A slider, a field and a label to drag; double-clicking the label puts the default back.
            const double fallback = initial;
            initial = settings.value(key).toDouble(initial);
            auto parameter = new ParameterControl(low, high, decimals, initial, fallback);
            addParameter(form, label, parameter, key + "Control");
            auto spin = parameter->spin();
            controls[key] = spin;
            parameters[key] = parameter;
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
                                   "Sample white balance", "Sample defringe",
                                   "Targeted: tone curve", "Targeted: color mixer"});
            auto previewTools = new QHBoxLayout;
            previewTools->addWidget(new QLabel("Preview tool"));
            previewTools->addWidget(previewTool);
            // What dragging on the picture changes in the color mixer.
            auto targetComponent = new QComboBox;
            targetComponent->setObjectName("cameraTargetComponent");
            targetComponent->addItems({"Hue", "Saturation", "Luminance"});
            targetComponent->setCurrentIndex(1);
            targetComponent->setVisible(false);
            previewTools->addWidget(targetComponent);
            layout->insertLayout(layout->count() - 1, previewTools);
            preview->setToolTip("Choose Sample point color and click the preview; choose Draw "
                                "geometry guides and drag up to four lines.");
            // The same tools work on the canvas, where the layer is shown at full size, and two more:
            // dragging up or down on the picture moves the tone curve for the tone there, or the
            // color mixer for the colors near the hue there.
            struct TargetDrag {
                double y = 0, tone = 0, hue = -1;
                QJsonObject start;
                std::optional<QPointF> from;
                bool active = false;
            };
            auto target = std::make_shared<TargetDrag>();
            auto normalized = [&](QPointF point) -> std::optional<QPointF> {
                const auto local = layer->placement(previewImage.size()).inverted().map(point);
                if (!QRectF(QPointF(), QSizeF(previewImage.size())).contains(local))
                    return std::nullopt;
                return QPointF(local.x() / previewImage.width(), 1 - local.y() / previewImage.height());
            };
            auto onCanvas = [&, preview, target, normalized,
                             targetComponent](const Canvas::PickEvent &e) {
                using Phase = Canvas::PickEvent::Phase;
                const int mode = preview->mode;
                const auto at = normalized(e.point);
                if (mode == 2) {
                    if (e.phase == Phase::Press)
                        target->from = at;
                    else if (e.phase == Phase::Release && target->from && at &&
                             QLineF(*target->from, *at).length() > .01)
                        preview->guided(QLineF(*target->from, *at));
                    return;
                }
                if (mode < 5) {
                    if (e.phase == Phase::Press && at)
                        preview->sampled(*at);
                    return;
                }
                if (e.phase == Phase::Press) {
                    target->active = false;
                    if (!at)
                        return;
                    const auto color = previewImage.pixelColor(
                        std::clamp(int(at->x() * previewImage.width()), 0, previewImage.width() - 1),
                        std::clamp(int((1 - at->y()) * previewImage.height()), 0,
                                   previewImage.height() - 1));
                    target->tone = .2126 * color.redF() + .7152 * color.greenF() + .0722 * color.blueF();
                    target->hue = color.hsvHueF() * 360;
                    target->y = e.point.y();
                    target->start = settings;
                    target->active = true;
                    return;
                }
                if (!target->active)
                    return;
                const double delta = (target->y - e.point.y()) * p->canvas->screenScale() * .35;
                if (mode == 5) {
                    const double tone = target->tone * 100;
                    const char *key =
                        tone < target->start.value("curveShadowSplit").toDouble(25)  ? "curveShadows"
                        : tone < target->start.value("curveDarkSplit").toDouble(50)  ? "curveDarks"
                        : tone < target->start.value("curveLightSplit").toDouble(75) ? "curveLights"
                                                                                     : "curveHighlights";
                    controls[key]->setValue(target->start.value(key).toDouble() + delta);
                } else if (target->hue >= 0) {
                    const QStringList families{"Reds",  "Oranges", "Yellows", "Greens",
                                               "Aquas", "Blues",   "Purples", "Magentas"};
                    const double centers[8] = {0, 30, 60, 120, 180, 240, 270, 300};
                    for (int i = 0; i < 8; ++i) {
                        double distance = std::abs(target->hue - centers[i]);
                        if (distance > 180)
                            distance = 360 - distance;
                        const double weight = std::max(0.0, 1 - distance / 40);
                        if (weight <= 0)
                            continue;
                        const auto key = "mixer" + families[i] + comboValue(targetComponent);
                        controls[key]->setValue(target->start.value(key).toDouble() + delta * weight);
                    }
                }
                if (e.phase == Phase::Release)
                    target->active = false;
            };
            connect(&dialog, &QDialog::finished, p->canvas, [p] { p->canvas->setPickHandler({}); });
            connect(previewTool, &QComboBox::currentIndexChanged, &dialog,
                    [&, preview, onCanvas, targetComponent](int mode) {
                        preview->mode = mode;
                        preview->setCursor(mode && mode < 5 ? Qt::CrossCursor : Qt::ArrowCursor);
                        targetComponent->setVisible(mode == 6);
                        if (mode)
                            p->canvas->setPickHandler(onCanvas);
                        else
                            p->canvas->setPickHandler({});
                        debounce.start(100);
                        preview->update();
                    });
            preview->sampled = [&, assignColor](QPointF p) {
                if (preview->mode >= 5)
                    return;
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
            {
                // A wheel per range sets its hue (angle) and saturation (distance) in one drag.
                auto wheels = new QWidget;
                auto grid = new QGridLayout(wheels);
                int index = 0;
                for (auto name : QStringList{"Shadows", "Midtones", "Highlights", "Global"}) {
                    auto wheel = new ColorWheel;
                    wheel->setObjectName("grade" + name + "Wheel");
                    auto hue = controls["grade" + name + "Hue"],
                         saturation = controls["grade" + name + "Saturation"];
                    wheel->hue = hue->value();
                    wheel->saturation = saturation->value();
                    wheel->changed = [hue, saturation](double h, double s) {
                        hue->setValue(h);
                        saturation->setValue(s);
                    };
                    auto follow = [wheel, hue, saturation] {
                        wheel->hue = hue->value();
                        wheel->saturation = saturation->value();
                        wheel->update();
                    };
                    connect(hue, &QDoubleSpinBox::valueChanged, wheel, follow);
                    connect(saturation, &QDoubleSpinBox::valueChanged, wheel, follow);
                    auto cell = new QVBoxLayout;
                    cell->addWidget(new QLabel(name), 0, Qt::AlignHCenter);
                    cell->addWidget(wheel, 0, Qt::AlignHCenter);
                    grid->addLayout(cell, index / 2, index % 2);
                    ++index;
                }
                form->insertRow(0, wheels);
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
            curveWidget = curve;
            curveChannels = channels;
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
        // Puts the selected range's (or channel's) values into the fields.
        std::function<void()> loadRange = [] {};
        if (rangeControl) {
            storeRange();
            loadRange = [&, rangeControl] {
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
                    };
            connect(rangeControl, &QComboBox::currentTextChanged, &dialog,
                    [&](const QString &) { loadRange(); });
        }
        if (kind == "Levels" || kind == "Curves" || kind == "Hue/Saturation") {
            tools = std::make_unique<AdjustmentDialog>(AdjustmentDialog{
                dialog, layout, form, settings, controls, parameters, rangeControl, loadRange,
                [&] { debounce.start(100); }, p, asAdjustment, editExisting, targetId});
            if (kind == "Levels")
                addLevelsTools(*tools);
            else if (kind == "Curves")
                addCurvesTools(*tools, curveWidget, curveChannels);
            else
                addHueSaturationTools(*tools);
        }
        if (live) {
            auto previewToggle = new QCheckBox("Preview");
            previewToggle->setObjectName("livePreviewControl");
            previewToggle->setChecked(true);
            previewToggle->setToolTip("Show the result on the canvas while adjusting.");
            layout->addWidget(previewToggle);
            connect(previewToggle, &QCheckBox::toggled, &dialog,
                    [&live](bool on) { live->setEnabled(on); });
        }
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        updatePreview();
        // However the dialog ends, the canvas goes back to the document before anything is applied.
        connect(&dialog, &QDialog::finished, &dialog, [&live] {
            if (live)
                live->stop();
        });
        const bool accepted = runLiveDialog(dialog, kind == "Camera Raw");
        live.reset();
        if (!accepted)
            return;
        // Nothing else could have been edited meanwhile; check all the same.
        require(page() == p && p->document.activeId() == targetId,
                "The layer changed while the dialog was open");
        layer = p->document.active();
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
                if (!p->canvas->session().selection.isNull()) {
                    l.mask = p->canvas->session().selection;
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
} // namespace compositor
