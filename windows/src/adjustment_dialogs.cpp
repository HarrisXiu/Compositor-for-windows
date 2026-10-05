// SPDX-License-Identifier: MIT
#include "adjustment_dialogs.h"
#include "adjustment_panels.h"
#include "curve_editor.h"
#include "editor.h"
#include "filters.h"
#include "language.h"
#include "parameter_control.h"
#include "render.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <vector>

namespace compositor {
namespace {
// The pixels the adjustment starts from: the layer for a filter, the picture under the
// adjustment layer for an adjustment. Its histogram is computed in the background; sampling a
// point reads the original color there, never the preview.
class OriginalPixels final : public QObject {
  public:
    OriginalPixels(const AdjustmentDialog &d, QObject *parent)
        : QObject(parent), page_(d.page), asAdjustment_(d.asAdjustment) {
        setObjectName("originalPixels");
        document_ = d.page->document;
        if (d.asAdjustment && d.editExisting)
            if (auto *edited = document_.find(d.layerId))
                edited->metadata["isVisible"] = false;
        if (const auto *layer = d.page->document.find(d.layerId))
            layer_ = *layer;
        const auto selection = d.page->session.selection;
        connect(&watcher_, &QFutureWatcher<Histogram>::finished, this, [this] {
            histogram = watcher_.result();
            ready = true;
            auto waiting = std::move(waiting_);
            for (auto &f : waiting)
                f();
        });
        watcher_.setFuture(QtConcurrent::run([document = document_, layer = layer_, selection,
                                              adjustment = asAdjustment_]() -> Histogram {
            QImage image, coverage;
            if (adjustment) {
                const auto size = document.size().scaled(1024, 1024, Qt::KeepAspectRatio)
                                      .boundedTo(document.size());
                image = renderDocument(document, size);
                if (!selection.isNull())
                    coverage = selection.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            } else {
                if (layer.image.isNull())
                    return {};
                image = layer.image;
                if (!selection.isNull())
                    coverage = layerSelection(document, layer, selection);
                if (std::max(image.width(), image.height()) > 2048) {
                    const auto size = image.size().scaled(2048, 2048, Qt::KeepAspectRatio);
                    image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    if (!coverage.isNull())
                        coverage = coverage.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                }
            }
            return levelsHistogram(image, coverage);
        }));
    }
    Histogram histogram{};
    bool ready = false;
    void whenReady(std::function<void()> f) {
        if (ready)
            f();
        else
            waiting_.push_back(std::move(f));
    }
    // The original color at a document point; invalid where there is none.
    QColor sample(QPointF point) const {
        const QPoint pixel(int(std::floor(point.x())), int(std::floor(point.y())));
        if (!asAdjustment_) {
            if (layer_.image.isNull())
                return {};
            const auto local = layer_.placement(layer_.image.size()).inverted().map(point);
            const QPoint at(int(std::floor(local.x())), int(std::floor(local.y())));
            if (!layer_.image.valid(at))
                return {};
            const auto color = layer_.image.pixelColor(at);
            return color.alpha() ? color : QColor();
        }
        if (!QRect(QPoint(), document_.size()).contains(pixel))
            return {};
        const auto image = renderArea(document_, {document_.size(), QRect(pixel, QSize(1, 1))});
        const auto color = image.pixelColor(0, 0);
        return color.alpha() ? color : QColor();
    }

  private:
    EditorPage *page_;
    bool asAdjustment_;
    Document document_;
    Layer layer_;
    QFutureWatcher<Histogram> watcher_;
    std::vector<std::function<void()>> waiting_;
};
OriginalPixels *originalPixels(const AdjustmentDialog &d) {
    if (auto existing = d.dialog.findChild<QObject *>("originalPixels", Qt::FindDirectChildrenOnly))
        return static_cast<OriginalPixels *>(existing);
    return new OriginalPixels(d, &d.dialog);
}
// Checkable buttons, at most one down, each a way of clicking on the canvas: while one is down
// the canvas sends its mouse gestures to `handler` with that button's index.
QHBoxLayout *pickButtons(const AdjustmentDialog &d, const QStringList &labels,
                         const QStringList &names,
                         std::function<void(int, const Canvas::PickEvent &)> handler) {
    auto row = new QHBoxLayout;
    auto group = new QButtonGroup(&d.dialog);
    group->setExclusive(false);
    for (int i = 0; i < labels.size(); ++i) {
        auto button = new QPushButton(labels[i]);
        button->setObjectName(names[i]);
        button->setCheckable(true);
        group->addButton(button, i);
        row->addWidget(button);
    }
    auto canvas = d.page->canvas;
    QObject::connect(group, &QButtonGroup::idToggled, &d.dialog, [group, canvas, handler](int id, bool on) {
        if (!on) {
            if (group->checkedId() < 0 || group->checkedButton() == nullptr)
                canvas->setPickHandler({});
            return;
        }
        for (auto button : group->buttons())
            if (group->id(button) != id && button->isChecked()) {
                QSignalBlocker block(button);
                button->setChecked(false);
            }
        canvas->setPickHandler([id, handler](const Canvas::PickEvent &e) { handler(id, e); });
    });
    QObject::connect(&d.dialog, &QDialog::finished, canvas, [canvas] { canvas->setPickHandler({}); });
    return row;
}
QString currentRange(const AdjustmentDialog &d) {
    return d.rangeControl ? comboValue(d.rangeControl) : QString("Master");
}
double rangeHue(const QString &range) {
    return std::max(0, int(hueRanges().indexOf(range)) - 1) * 60.0;
}
QColor hueColor(double degrees, double saturation = .85, double value = .9) {
    degrees = std::fmod(std::fmod(degrees, 360) + 360, 360);
    return QColor::fromHsvF(float(degrees / 360), float(saturation), float(value));
}
} // namespace

void addLevelsTools(const AdjustmentDialog &d) {
    auto graph = new LevelsGraph;
    graph->setObjectName("levelsGraph");
    d.layout->insertWidget(0, graph);
    auto sync = [d, graph] {
        graph->channel = d.rangeControl ? d.rangeControl->currentIndex() : 0;
        graph->black = d.controls.value("inputBlack")->value();
        graph->gamma = d.controls.value("gamma")->value();
        graph->white = d.controls.value("inputWhite")->value();
        graph->outputBlack = d.controls.value("outputBlack")->value();
        graph->outputWhite = d.controls.value("outputWhite")->value();
        graph->update();
    };
    graph->changed = [d](const QString &key, double value) {
        if (auto control = d.controls.value(key))
            control->setValue(value);
    };
    for (auto key : {"inputBlack", "gamma", "inputWhite", "outputBlack", "outputWhite"})
        QObject::connect(d.controls.value(key), &QDoubleSpinBox::valueChanged, graph, sync);
    // After the fields have taken the channel's values.
    if (d.rangeControl)
        QObject::connect(d.rangeControl, &QComboBox::currentTextChanged, graph, sync);
    sync();

    auto pixels = originalPixels(d);
    auto automatic = new QHBoxLayout;
    automatic->addWidget(new QLabel("Auto"));
    const QStringList names{"levelsAutoContrast", "levelsAutoColor", "levelsAutoNeutral"};
    const QStringList labels{"Contrast", "Color", "Color + neutral midtones"};
    QList<QPushButton *> autoButtons;
    for (int i = 0; i < 3; ++i) {
        auto button = new QPushButton(labels[i]);
        button->setObjectName(names[i]);
        button->setEnabled(false);
        autoButtons << button;
        automatic->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, graph, [d, pixels, sync, i] {
            d.settings["ranges"] = autoLevels(pixels->histogram, AutoLevels(i));
            d.reloadRange();
            sync();
        });
    }
    pixels->whenReady([graph, pixels, autoButtons] {
        graph->histogram = pixels->histogram;
        graph->histogramReady = true;
        graph->update();
        for (auto button : autoButtons)
            button->setEnabled(true);
    });
    d.layout->insertLayout(1, automatic);
    auto samplers = pickButtons(
        d, {"Black point", "Gray point", "White point"},
        {"levelsSampleBlack", "levelsSampleGray", "levelsSampleWhite"},
        [d, pixels, sync](int mode, const Canvas::PickEvent &e) {
            if (e.phase != Canvas::PickEvent::Phase::Press)
                return;
            const auto color = pixels->sample(e.point);
            if (!color.isValid())
                return;
            d.settings["ranges"] =
                sampleLevels(d.settings.value("ranges").toArray(), color, LevelsSample(mode));
            d.reloadRange();
            sync();
        });
    samplers->insertWidget(0, new QLabel("Sample"));
    d.layout->insertLayout(2, samplers);
    auto hint = new QLabel(d.asAdjustment ? "Eyedroppers read the picture under the adjustment."
                                          : "Eyedroppers read the layer's original pixels.");
    hint->setWordWrap(true);
    d.layout->insertWidget(3, hint);
}

void addCurvesTools(const AdjustmentDialog &d, CurveEditor *curve, QComboBox *channels) {
    curve->setObjectName("curveEditor");
    auto row = new QHBoxLayout;
    auto readout = new QLabel;
    readout->setObjectName("curvePointReadout");
    readout->setMinimumWidth(150);
    auto remove = new QPushButton("Remove point");
    remove->setObjectName("curveRemovePoint");
    remove->setEnabled(false);
    auto reset = new QPushButton("Reset curve");
    reset->setObjectName("curveReset");
    row->addWidget(readout, 1);
    row->addWidget(remove);
    row->addWidget(reset);
    d.layout->insertLayout(d.layout->indexOf(curve) + 1, row);
    curve->selectionChanged = [curve, readout, remove](int i) {
        if (i < 0 || i >= curve->points.size()) {
            readout->clear();
            remove->setEnabled(false);
            return;
        }
        const auto point = curve->points[i].toObject();
        readout->setText(uiText("Input") + QString(" %1 · ").arg(std::lround(point.value("x").toDouble())) +
                         uiText("Output") + QString(" %1").arg(std::lround(point.value("y").toDouble())));
        remove->setEnabled(i > 0 && i + 1 < curve->points.size());
    };
    QObject::connect(remove, &QPushButton::clicked, curve, [curve] { curve->removeSelected(); });
    QObject::connect(reset, &QPushButton::clicked, curve, [curve] { curve->resetCurve(); });
    auto pixels = originalPixels(d);
    auto showHistogram = [curve, channels, pixels] {
        if (!pixels->ready)
            return;
        const int channel = std::clamp(channels->currentIndex(), 0, 3);
        const QColor colors[4] = {QColor(90, 90, 90), QColor(120, 50, 50), QColor(50, 110, 60),
                                  QColor(50, 75, 130)};
        curve->histogram = pixels->histogram[size_t(channel)];
        curve->histogramColor = colors[channel];
        curve->showHistogram = true;
        curve->update();
    };
    pixels->whenReady(showHistogram);
    QObject::connect(channels, &QComboBox::currentIndexChanged, curve, [curve, showHistogram] {
        curve->selected = -1;
        if (curve->selectionChanged)
            curve->selectionChanged(-1);
        showHistogram();
    });
}

void addHueSaturationTools(const AdjustmentDialog &d) {
    auto spectrum = new HueSpectrum;
    spectrum->setObjectName("hueSpectrum");
    d.form->addRow(spectrum);
    auto invert = new QCheckBox("Apply outside this range instead");
    invert->setObjectName("invertRangeControl");
    invert->setChecked(d.settings.value("invertRange").toBool());
    d.form->addRow(invert);
    QObject::connect(invert, &QCheckBox::toggled, &d.dialog, [d](bool on) {
        d.settings["invertRange"] = on;
        d.changed();
    });
    spectrum->changed = [d](const QJsonObject &band) {
        auto bands = d.settings.value("bands").toObject();
        bands[currentRange(d)] = band;
        d.settings["bands"] = bands;
        d.changed();
    };
    auto pixels = originalPixels(d);
    // Targeted adjustment: where the drag started, and the value it started from.
    struct Target {
        double x = 0, start = 0;
        QString key;
    };
    auto target = std::make_shared<Target>();
    auto canvas = d.page->canvas;
    auto samplers = pickButtons(
        d, {"Sample", "Add to range", "Subtract from range", "Targeted adjustment"},
        {"hueSample", "hueAdd", "hueSubtract", "hueTargeted"},
        [d, pixels, spectrum, target, canvas](int mode, const Canvas::PickEvent &e) {
            if (mode == 3) {
                if (e.phase == Canvas::PickEvent::Phase::Press) {
                    const auto color = pixels->sample(e.point);
                    target->key.clear();
                    if (!color.isValid() || color.hslHueF() < 0)
                        return;
                    // The drag adjusts the range the clicked color belongs to.
                    if (d.rangeControl)
                        selectComboValue(d.rangeControl, nearestHueRange(color.hslHueF() * 360));
                    target->key = (e.modifiers & Qt::ControlModifier) ? "hue" : "saturation";
                    target->x = e.point.x();
                    target->start = d.controls.value(target->key)->value();
                } else if (!target->key.isEmpty()) {
                    const double moved = (e.point.x() - target->x) * canvas->screenScale();
                    d.controls.value(target->key)->setValue(target->start + moved / 2);
                    if (e.phase == Canvas::PickEvent::Phase::Release)
                        target->key.clear();
                }
                return;
            }
            if (e.phase != Canvas::PickEvent::Phase::Press || currentRange(d) == "Master")
                return;
            const auto color = pixels->sample(e.point);
            if (!color.isValid() || color.hslHueF() < 0)
                return;
            const double hue = color.hslHueF() * 360;
            const auto band = hueBand(d.settings, currentRange(d));
            spectrum->band = mode == 0   ? hueBandAround(hue)
                             : mode == 1 ? widenHueBand(band, hue)
                                         : narrowHueBand(band, hue);
            spectrum->changed(spectrum->band);
            spectrum->update();
        });
    d.form->addRow(samplers);
    auto colorize = d.dialog.findChild<QCheckBox *>("colorizeControl");
    auto visibility = [d, spectrum, invert, samplers, colorize] {
        const bool colorizing = colorize && colorize->isChecked();
        const bool band = !colorizing && currentRange(d) != "Master";
        spectrum->setVisible(band);
        invert->setVisible(band);
        for (int i = 0; i < samplers->count(); ++i)
            if (auto widget = samplers->itemAt(i)->widget())
                widget->setVisible(i == 3 ? !colorizing : band);
        if (d.rangeControl)
            d.rangeControl->setEnabled(!colorizing);
    };
    if (d.rangeControl)
        QObject::connect(d.rangeControl, &QComboBox::currentIndexChanged, spectrum, [d, visibility] {
            visibility();
            refreshAdjustmentTools(d, "Hue/Saturation");
        });
    if (colorize)
        QObject::connect(colorize, &QCheckBox::toggled, spectrum, [d, visibility](bool on) {
            // Photoshop starts colorizing from red at 25% saturation.
            d.controls.value("hue")->setValue(0);
            d.controls.value("saturation")->setValue(on ? 25 : 0);
            d.controls.value("lightness")->setValue(0);
            visibility();
            refreshAdjustmentTools(d, "Hue/Saturation");
        });
    visibility();
    refreshAdjustmentTools(d, "Hue/Saturation");
}

void refreshAdjustmentTools(const AdjustmentDialog &d, const QString &kind) {
    if (kind != "Hue/Saturation")
        return;
    const auto range = currentRange(d);
    const bool colorizing = d.settings.value("colorize").toBool();
    if (auto spectrum = static_cast<HueSpectrum *>(d.dialog.findChild<QWidget *>("hueSpectrum"))) {
        spectrum->band = hueBand(d.settings, range);
        QImage ramp(360, 1, QImage::Format_RGBA8888_Premultiplied);
        for (int x = 0; x < 360; ++x)
            ramp.setPixelColor(x, 0, QColor::fromHsvF(float(x / 360.0), 1, 1));
        try {
            spectrum->after = applyFilter(ramp, "Hue/Saturation", d.settings);
        } catch (const std::exception &) {
            spectrum->after = ramp;
        }
        spectrum->update();
    }
    const double center = rangeHue(range);
    if (auto hue = d.parameters.value("hue")) {
        QList<QColor> stops;
        for (int i = -180; i <= 180; i += 30)
            stops << hueColor((colorizing ? 180 : center) + i);
        hue->setTrack(stops);
    }
    if (auto saturation = d.parameters.value("saturation")) {
        const QColor gray(140, 140, 143);
        saturation->setTrack({gray, colorizing ? hueColor(d.settings.value("hue").toDouble())
                                    : range == "Master" ? QColor(220, 46, 51)
                                                        : hueColor(center)});
    }
    if (auto lightness = d.parameters.value("lightness"))
        lightness->setTrack({Qt::black, Qt::white});
}
} // namespace compositor
