// SPDX-License-Identifier: MIT
#include "editable_layers.h"
#include "editor.h"
#include "language.h"
#include <QAbstractTextDocumentLayout>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QJsonArray>
#include <QPushButton>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QVBoxLayout>

namespace compositor {
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
} // namespace compositor
