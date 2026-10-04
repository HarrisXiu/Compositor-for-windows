// SPDX-License-Identifier: MIT
#include "editor.h"
#include "language.h"
#include "shortcuts.h"
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolBar>
#include <QWidgetAction>
namespace compositor {
void EditorWindow::buildExtraToolOptions() {
    auto tools = findChild<QToolBar *>("canvasTools");
    auto zoom = tools->addAction("Zoom");
    zoom->setProperty("canvasTool", int(Tool::Zoom));
    zoom->setCheckable(true);
    zoom->setShortcut(QKeySequence("Z"));
    if (auto move = tools->actions().front(); move->actionGroup())
        move->actionGroup()->addAction(zoom);
    connect(zoom, &QAction::triggered, this, [this] { setTool(Tool::Zoom); });
    auto select = tools->addAction("Select");
    select->setProperty("canvasTool", int(Tool::Select));
    select->setCheckable(true);
    select->setShortcut(QKeySequence("A"));
    if (auto move = tools->actions().front(); move->actionGroup())
        move->actionGroup()->addAction(select);
    connect(select, &QAction::triggered, this, [this] { setTool(Tool::Select); });
    for (auto a : tools->actions())
        if (a->property("canvasTool").isValid()) {
            Shortcuts::instance().registerAction(
                a, "tool/" + QString::number(a->property("canvasTool").toInt()), "Tools");
            // These toolbar keys retain the selected mode; Tab cycles it explicitly.
        }
    auto toolbar = addToolBar("Tool Options");
    toolbar->setObjectName("extraToolOptions");
    toolbar->setMovable(false);
    auto add = [toolbar](const QString &label, QWidget *widget, const QVector<Tool> &tools) {
        QVariantList kinds;
        for (auto t : tools)
            kinds << int(t);
        if (!label.isEmpty()) {
            auto a = toolbar->addWidget(new QLabel("  " + label + "  "));
            a->setProperty("optionTools", kinds);
        }
        auto a = toolbar->addWidget(widget);
        a->setProperty("optionTools", kinds);
    };
    auto boolean = [this, &add](const QString &label, const QString &name,
                                bool EditorSession::*member, const QVector<Tool> &tools) {
        auto widget = new QCheckBox(label);
        widget->setObjectName(name);
        add({}, widget, tools);
        connect(widget, &QCheckBox::toggled, this, [this, member](bool value) {
            if (!syncing_ && page())
                page()->session.*member = value;
        });
        return widget;
    };
    auto number = [this, &add](const QString &label, const QString &name,
                               double EditorSession::*member, double min, double max,
                               const QVector<Tool> &tools) {
        auto widget = new QDoubleSpinBox;
        widget->setObjectName(name);
        widget->setRange(min, max);
        widget->setMaximumWidth(95);
        add(label, widget, tools);
        connect(widget, &QDoubleSpinBox::valueChanged, this, [this, member](double value) {
            if (!syncing_ && page())
                page()->session.*member = value;
        });
        return widget;
    };
    autoSelect_ = boolean("Auto Select", "autoSelect", &EditorSession::autoSelect, {Tool::Move});
    number("Smoothing", "brushSmoothing", &EditorSession::brushSmoothing, 0, 100,
           {Tool::Brush, Tool::Erase});
    boolean("Anti-alias", "selectionAntialiased", &EditorSession::selectionAntialiased,
            {Tool::EllipseSelect, Tool::Lasso});
    boolean("Polygonal", "polygonalLasso", &EditorSession::polygonalLasso, {Tool::Lasso});
    findChild<QCheckBox *>("polygonalLasso")->setToolTip(
        "Click vertices; Enter, double-click, or click the first vertex to close. Backspace removes a vertex; Esc cancels.");
    boolean("Sample All Layers", "wandMerged", &EditorSession::wandMerged, {Tool::Wand});
    auto integer = [this, &add](const QString &label, const QString &name,
                                int EditorSession::*member, const QStringList &items,
                                const QVector<Tool> &tools) {
        auto widget = new QComboBox;
        widget->setObjectName(name);
        widget->addItems(items);
        add(label, widget, tools);
        connect(widget, &QComboBox::currentIndexChanged, this, [this, member](int value) {
            if (!syncing_ && page())
                page()->session.*member = value;
        });
        return widget;
    };
    integer("Type", "healingMode", &EditorSession::healingMode,
            {"Content-Aware", "Create Texture", "Proximity Match"}, {Tool::Heal});
    auto wandSize = new QComboBox;
    wandSize->setObjectName("wandSampleSize");
    wandSize->addItems({"Point sample", "3 × 3 average", "5 × 5 average", "11 × 11 average"});
    add("Sample size", wandSize, {Tool::Wand});
    connect(wandSize, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int sizes[]{1, 3, 5, 11};
        if (!syncing_ && page())
            page()->session.wandSampleSize = sizes[index];
    });
    auto mode = [this, &add](const QString &name, const QString &label, const QStringList &items,
                             const QVector<Tool> &kinds) {
        auto widget = new QComboBox;
        widget->setObjectName(name);
        widget->addItems(items);
        add(label, widget, kinds);
        connect(widget, &QComboBox::currentIndexChanged, this, [this, kinds](int index) {
            if (!syncing_ && page())
                setTool(kinds[index]);
        });
    };
    mode("brushMode", "Mode", {"Paint", "Erase"}, {Tool::Brush, Tool::Erase});
    mode("smearMode", "Mode", {"Blur", "Smudge", "Liquify"},
         {Tool::Blur, Tool::Smudge, Tool::Liquify});
    mode("shapeKind", "Shape", {"Rectangle", "Ellipse", "Line"},
         {Tool::Rectangle, Tool::Ellipse, Tool::Line});
    mode("marqueeKind", "Shape", {"Rectangle", "Ellipse"},
         {Tool::RectangleSelect, Tool::EllipseSelect});
    auto handles = new QCheckBox("Show Transform Controls");
    handles->setObjectName("transformControls");
    handles->setChecked(loadCanvasViewOptions().transformControls);
    add({}, handles, {Tool::Move});
    connect(handles, &QCheckBox::toggled, this, [this](bool checked) {
        if (syncing_)
            return;
        for (auto a : findChildren<QAction *>())
            if (a->property("layerAction").toString() == "Show Transform Controls") {
                a->setChecked(checked);
                return;
            }
    });
    selectionMode_ = new QComboBox;
    selectionMode_->setObjectName("selectionMode");
    selectionMode_->addItems({"Replace", "Add", "Subtract", "Intersect"});
    add("Selection mode", selectionMode_,
        {Tool::RectangleSelect, Tool::EllipseSelect, Tool::Lasso, Tool::Wand});
    connect(selectionMode_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!syncing_ && page())
            page()->session.selectionMode = index;
    });
    cloneAligned_ = boolean("Aligned", "cloneAligned", &EditorSession::cloneAligned, {Tool::Clone});
    cloneMerged_ =
        boolean("Sample All Layers", "cloneMerged", &EditorSession::cloneMerged, {Tool::Clone});
    gradientType_ = new QComboBox;
    gradientType_->setObjectName("gradientType");
    gradientType_->addItems({"Linear", "Radial"});
    add("Gradient type", gradientType_, {Tool::Gradient});
    connect(gradientType_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!syncing_ && page())
            page()->session.gradientKind = index;
    });
    gradientBackground_ = boolean("Foreground to Background", "gradientBackground",
                                  &EditorSession::gradientBackground, {Tool::Gradient});
    gradientReverse_ =
        boolean("Reverse", "gradientReverse", &EditorSession::gradientReverse, {Tool::Gradient});
    cornerRadius_ = number("Corner radius", "shapeCornerRadius", &EditorSession::shapeCornerRadius,
                           0, 2000, {Tool::Rectangle});
    lineWidth_ = number("Line width", "shapeLineWidth", &EditorSession::shapeLineWidth, .1, 2000,
                        {Tool::Line});
    textSize_ = number("Font size", "textSize", &EditorSession::textSize, 1, 2000, {Tool::Text});
    auto font = new QFontComboBox;
    textFont_ = font;
    font->setObjectName("textFont");
    add("Font", font, {Tool::Text});
    connect(font, &QFontComboBox::currentFontChanged, this, [this](const QFont &value) {
        if (!syncing_ && page())
            page()->session.textFont = value.family();
    });
    font->setMaximumWidth(180);
    auto alignment = new QComboBox;
    alignment->setObjectName("textAlignment");
    alignment->addItems({"Left", "Center", "Right"});
    add("Alignment", alignment, {Tool::Text});
    connect(alignment, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QStringList alignments{"Left", "Center", "Right"};
        if (!syncing_ && page())
            page()->session.textAlignment = alignments[index];
    });
    number("Tracking", "textTracking", &EditorSession::textTracking, -100, 1000, {Tool::Text});
    number("Leading", "textLeading", &EditorSession::textLeading, 0, 5000, {Tool::Text});
    auto editTextButton = new QPushButton("Edit Text");
    add({}, editTextButton, {Tool::Text});
    connect(editTextButton, &QPushButton::clicked, this, [this] {
        if (!page())
            return;
        try {
            page()->canvas->beginTextEditing(page()->document.activeId());
        } catch (const std::exception &error) {
            showError(QString::fromUtf8(error.what()));
        }
    });
    pickerSize_ = new QComboBox;
    pickerSize_->setObjectName("pickerSize");
    pickerSize_->addItems({"Point sample", "3 × 3 average", "5 × 5 average", "11 × 11 average"});
    add("Sample size", pickerSize_, {Tool::Eyedropper});
    connect(pickerSize_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int sizes[]{1, 3, 5, 11};
        if (!syncing_ && page())
            page()->session.pickerSize = sizes[index];
    });
    zoomPercent_ = new QDoubleSpinBox;
    zoomPercent_->setObjectName("zoomPercent");
    zoomPercent_->setRange(1, 6400);
    zoomPercent_->setSuffix(" %");
    add("Zoom", zoomPercent_, {Tool::Pan, Tool::Zoom});
    connect(zoomPercent_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!syncing_ && page())
            page()->canvas->zoomTo(value / 100);
    });
    auto fit = new QPushButton("Fit Canvas");
    add({}, fit, {Tool::Pan, Tool::Zoom});
    connect(fit, &QPushButton::clicked, this, [this] {
        if (page()) {
            page()->canvas->fit();
            syncToolOptions();
        }
    });
    auto actual = new QPushButton("Actual Pixels");
    add({}, actual, {Tool::Pan, Tool::Zoom});
    connect(actual, &QPushButton::clicked, this, [this] {
        if (page())
            page()->canvas->zoomTo(1);
    });
    auto hint = new QLabel("Alt-click sets the source");
    add({}, hint, {Tool::Clone});
    for (auto action : findChild<QToolBar *>("brushOptions")->actions())
        if (auto widgetAction = qobject_cast<QWidgetAction *>(action)) {
            auto widget = widgetAction->defaultWidget();
            auto label = qobject_cast<QLabel *>(widget);
            auto text = label ? label->text().trimmed() : QString();
            action->setProperty("brushOnly", widget == brushSize_ || widget == hardness_ ||
                                                 text == "Size" || text == "Hardness");
            action->setProperty("blurOnly", widget == blurRadius_ || text == "Blur radius");
            action->setProperty("maskOnly", widget == maskTarget_);
            action->setProperty("opacityOnly", widget == brushOpacity_ || text == "Opacity");
        }
}
void EditorWindow::syncExtraToolOptions() {
    if (!page() || !selectionMode_)
        return;
    auto &s = page()->session;
    const bool brush = page()->canvas->brushTool();
    auto bar = findChild<QToolBar *>("brushOptions");
    bar->setVisible(brush || s.tool == Tool::Gradient || s.tool == Tool::Text ||
                    s.tool == Tool::Rectangle || s.tool == Tool::Ellipse || s.tool == Tool::Line);
    for (auto action : bar->actions()) {
        bool show = true;
        if (action->property("brushOnly").toBool())
            show = brush;
        if (action->property("blurOnly").toBool())
            show = s.tool == Tool::Blur;
        if (action->property("opacityOnly").toBool())
            show = brush || s.tool == Tool::Gradient;
        if (action->property("maskOnly").toBool())
            show = s.tool == Tool::Brush || s.tool == Tool::Erase || s.tool == Tool::Blur ||
                   s.tool == Tool::Gradient;
        action->setVisible(show);
    }
    findChild<QToolBar *>("wandOptions")->setVisible(s.tool == Tool::Wand);
    auto extra = findChild<QToolBar *>("extraToolOptions");
    bool visible = false;
    for (auto a : extra->actions()) {
        bool show = a->property("optionTools").toList().contains(QVariant(int(s.tool)));
        a->setVisible(show);
        visible |= show;
    }
    extra->setVisible(visible);
    const QSignalBlocker b1(selectionMode_), b2(cloneAligned_), b3(cloneMerged_), b4(gradientType_),
        b5(gradientBackground_), b6(gradientReverse_), b7(cornerRadius_), b8(lineWidth_),
        b9(textFont_), b10(textSize_), b11(pickerSize_), b12(zoomPercent_), b13(autoSelect_);
    selectionMode_->setCurrentIndex(s.selectionMode);
    cloneAligned_->setChecked(s.cloneAligned);
    cloneMerged_->setChecked(s.cloneMerged);
    gradientType_->setCurrentIndex(s.gradientKind);
    gradientBackground_->setChecked(s.gradientBackground);
    gradientReverse_->setChecked(s.gradientReverse);
    cornerRadius_->setValue(s.shapeCornerRadius);
    lineWidth_->setValue(s.shapeLineWidth);
    textSize_->setValue(s.textSize);
    qobject_cast<QFontComboBox *>(textFont_)->setCurrentFont(QFont(s.textFont));
    autoSelect_->setChecked(s.autoSelect);
    const int sizes[]{1, 3, 5, 11};
    for (int i = 0; i < 4; ++i)
        if (s.pickerSize == sizes[i])
            pickerSize_->setCurrentIndex(i);
    auto combo = [this](const QString &name, int index) {
        auto widget = findChild<QComboBox *>(name);
        QSignalBlocker blocker(widget);
        widget->setCurrentIndex(index);
    };
    auto number = [this](const QString &name, double value) {
        auto widget = findChild<QDoubleSpinBox *>(name);
        QSignalBlocker blocker(widget);
        widget->setValue(value);
    };
    number("brushSmoothing", s.brushSmoothing);
    number("textTracking", s.textTracking);
    number("textLeading", s.textLeading);
    combo("healingMode", s.healingMode);
    combo("textAlignment", QStringList{"Left", "Center", "Right"}.indexOf(s.textAlignment));
    for (int i = 0; i < 4; ++i)
        if (s.wandSampleSize == sizes[i])
            combo("wandSampleSize", i);
    for (auto name : {"selectionAntialiased", "wandMerged", "polygonalLasso"}) {
        auto widget = findChild<QCheckBox *>(name);
        QSignalBlocker blocker(widget);
        widget->setChecked(QString(name) == "wandMerged" ? s.wandMerged
                              : QString(name) == "polygonalLasso" ? s.polygonalLasso
                                                                  : s.selectionAntialiased);
    }
    combo("brushMode", s.tool == Tool::Erase ? 1 : 0);
    combo("smearMode", s.tool == Tool::Smudge ? 1 : s.tool == Tool::Liquify ? 2 : 0);
    combo("shapeKind", s.tool == Tool::Ellipse ? 1 : s.tool == Tool::Line ? 2 : 0);
    combo("marqueeKind", s.tool == Tool::EllipseSelect ? 1 : 0);
    zoomPercent_->setValue(page()->canvas->zoom * 100);
    if (auto handles = findChild<QCheckBox *>("transformControls")) {
        QSignalBlocker blocker(handles);
        handles->setChecked(s.view.transformControls);
    }
}
} // namespace compositor
