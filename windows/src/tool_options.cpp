// SPDX-License-Identifier: MIT
#include "editor.h"
#include "language.h"
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolBar>

namespace compositor {
void EditorWindow::buildToolOptions() {
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
        a->setProperty("canvasTool", int(option.tool));
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
    blurRadius_ = new QDoubleSpinBox;
    blurRadius_->setRange(.5, 50);
    blurRadius_->setValue(5);
    blurRadius_->setToolTip("Blur brush radius in canvas pixels");
    brush->addWidget(new QLabel("  Blur radius  "));
    brush->addWidget(blurRadius_);
    connect(blurRadius_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!syncing_ && page())
            page()->canvas->session().blurRadius = v;
    });
    colorButton_ = new QPushButton("Foreground color");
    backgroundButton_ = new QPushButton("Background color");
    colorButton_->setObjectName("foregroundColor");
    backgroundButton_->setObjectName("backgroundColor");
    brush->addWidget(colorButton_);
    brush->addWidget(backgroundButton_);
    auto chooseColor = [this](bool background) {
        auto p = page();
        if (!p)
            return;
        auto initial = background ? p->session.background : p->session.foreground;
        auto c =
            QColorDialog::getColor(initial, this, "Paint Color", QColorDialog::ShowAlphaChannel);
        if (c.isValid()) {
            (background ? p->session.background : p->session.foreground) = c;
            syncToolOptions();
        }
    };
    connect(colorButton_, &QPushButton::clicked, this, [chooseColor] { chooseColor(false); });
    connect(backgroundButton_, &QPushButton::clicked, this, [chooseColor] { chooseColor(true); });
    maskTarget_ = new QCheckBox("Paint mask");
    brush->addWidget(maskTarget_);
    auto syncBrush = [this] {
        if (!syncing_ && page()) {
            auto c = page()->canvas;
            c->session().brushSize = brushSize_->value();
            c->session().hardness = hardness_->value() / 100;
            c->session().brushOpacity = brushOpacity_->value() / 100;
            c->session().target = maskTarget_->isChecked() ? EditTarget::Mask : EditTarget::Pixels;
        }
    };
    connect(brushSize_, &QDoubleSpinBox::valueChanged, this, syncBrush);
    connect(hardness_, &QDoubleSpinBox::valueChanged, this, syncBrush);
    connect(brushOpacity_, &QDoubleSpinBox::valueChanged, this, syncBrush);
    connect(maskTarget_, &QCheckBox::toggled, this, syncBrush);
    auto selectionTools = addToolBar("Selection Options");
    selectionTools->setMovable(false);
    selectionTools->addWidget(new QLabel("Wand tolerance  "));
    tolerance_ = new QDoubleSpinBox;
    tolerance_->setRange(0, 255);
    tolerance_->setValue(32);
    selectionTools->addWidget(tolerance_);
    contiguous_ = new QCheckBox("Contiguous");
    contiguous_->setChecked(true);
    selectionTools->addWidget(contiguous_);
    connect(tolerance_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!syncing_ && page())
            page()->canvas->session().wandTolerance = v;
    });
    connect(contiguous_, &QCheckBox::toggled, this, [this](bool v) {
        if (!syncing_ && page())
            page()->canvas->session().wandContiguous = v;
    });
}
void EditorWindow::syncToolOptions() {
    auto p = page();
    if (!p)
        return;
    tool_ = p->session.tool;
    for (auto action : findChildren<QAction *>())
        if (action->property("canvasTool").isValid())
            action->setChecked(action->property("canvasTool").toInt() == int(tool_));
    QSignalBlocker sizeBlock(brushSize_), hardnessBlock(hardness_), opacityBlock(brushOpacity_),
        maskBlock(maskTarget_), blurBlock(blurRadius_), toleranceBlock(tolerance_),
        contiguousBlock(contiguous_);
    brushSize_->setValue(p->session.brushSize);
    hardness_->setValue(p->session.hardness * 100);
    brushOpacity_->setValue(p->session.brushOpacity * 100);
    blurRadius_->setValue(p->session.blurRadius);
    tolerance_->setValue(p->session.wandTolerance);
    contiguous_->setChecked(p->session.wandContiguous);
    maskTarget_->setChecked(p->session.target == EditTarget::Mask);
    colorButton_->setStyleSheet("background:" + p->session.foreground.name() + ";color:white;");
    backgroundButton_->setStyleSheet("background:" + p->session.background.name() +
                                     ";color:white;");
}
} // namespace compositor
