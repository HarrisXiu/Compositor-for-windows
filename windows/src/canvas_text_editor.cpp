// SPDX-License-Identifier: MIT
#include "canvas_text_editor.h"
#include "document.h"
#include "editable_layers.h"
#include "language.h"
#include "text_fonts.h"
#include <QAction>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <memory>

namespace compositor {
namespace {
class TextInput final : public QTextEdit {
  public:
    std::function<void()> apply, cancel;
    bool composing = false;

  protected:
    void inputMethodEvent(QInputMethodEvent *event) override {
        composing = !event->preeditString().isEmpty();
        QTextEdit::inputMethodEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            if (composing) {
                QGuiApplication::inputMethod()->reset();
                QInputMethodEvent reset;
                inputMethodEvent(&reset);
            } else if (cancel)
                cancel();
            event->accept();
        } else if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                   event->modifiers() == Qt::ControlModifier) {
            if (apply)
                apply();
            event->accept();
        } else
            QTextEdit::keyPressEvent(event);
    }
};
} // namespace
CanvasTextEditor::CanvasTextEditor(const QJsonObject &style, QWidget *parent)
    : QGraphicsView(parent), base_(style) {
    setObjectName("inlineTextView");
    setFrameShape(QFrame::NoFrame);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setStyleSheet("QGraphicsView { background: transparent; border: 0; }");
    setAttribute(Qt::WA_TranslucentBackground);
    viewport()->setAttribute(Qt::WA_TranslucentBackground);
    auto scene = new QGraphicsScene(this);
    setScene(scene);
    auto input = new TextInput;
    std::unique_ptr<TextInput> inputOwner(input);
    text_ = input;
    text_->setObjectName("inlineTextInput");
    text_->setFrameShape(QFrame::NoFrame);
    text_->setStyleSheet("QTextEdit { background: transparent; border: 0; padding: 0; }");
    text_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    text_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Keep glyphs on the saved source grid, even when the caret enters overflow text.
    for (auto bar : {text_->horizontalScrollBar(), text_->verticalScrollBar()})
        connect(bar, &QScrollBar::valueChanged, this, [bar] {
            QSignalBlocker blocker(bar);
            bar->setValue(0);
        });
    text_->setTabChangesFocus(false);
    text_->setAcceptRichText(false);
    text_->setLineWrapMode(style.contains("boxSize") ? QTextEdit::WidgetWidth : QTextEdit::NoWrap);
    loadTextDocument(*text_->document(), style);
    text_->document()->clearUndoRedoStacks();
    proxy_ = scene->addWidget(text_);
    inputOwner.release();
    input->apply = [this] {
        if (apply)
            apply();
    };
    input->cancel = [this] {
        if (cancel)
            cancel();
    };
    connect(text_, &QTextEdit::textChanged, this, [this] {
        if (changed)
            changed();
    });
    toolbar_ = new QToolBar(parent);
    toolbar_->setObjectName("inlineTextToolbar");
    toolbar_->setMovable(false);
    auto font = new QFontComboBox;
    font->setObjectName("inlineTextFont");
    font->setMaximumWidth(150);
    font->setCurrentFont(QFont(resolvedTextFont(style.value("fontName").toString())));
    font->setToolTip(style.value("fontName").toString() + " → " + font->currentFont().family());
    toolbar_->addWidget(font);
    connect(font, &QFontComboBox::currentFontChanged, this, [this](const QFont &font) {
        QGuiApplication::inputMethod()->commit();
        auto cursor = text_->textCursor();
        const bool all = !cursor.hasSelection();
        if (all) {
            cursor.select(QTextCursor::Document);
            base_["fontName"] = font.family();
        }
        QTextCharFormat format;
        format.setFontFamilies({font.family()});
        format.setProperty(RequestedFontProperty, font.family());
        cursor.mergeCharFormat(format);
        if (all)
            text_->mergeCurrentCharFormat(format);
        if (changed)
            changed();
        text_->setFocus();
    });
    auto spin = [&](const QString &name, const QString &tip, double min, double max, double value) {
        auto field = new QDoubleSpinBox;
        field->setObjectName(name);
        field->setToolTip(tip);
        field->setRange(min, max);
        field->setValue(value);
        field->setMaximumWidth(70);
        toolbar_->addWidget(field);
        return field;
    };
    auto size = spin("inlineTextSize", "Font size", 1, 2000, style.value("fontSize").toDouble(48));
    auto tracking =
        spin("inlineTextTracking", "Tracking", -100, 1000, style.value("tracking").toDouble());
    auto leading = spin("inlineTextLeading", "Leading", 0, 5000, style.value("leading").toDouble());
    auto alignment = new QComboBox;
    alignment->setObjectName("inlineTextAlignment");
    alignment->addItems({"Left", "Center", "Right"});
    alignment->setCurrentIndex(std::max(0, int(QStringList{"Left", "Center", "Right"}.indexOf(
                                               style.value("alignment").toString("Left")))));
    toolbar_->addWidget(alignment);
    auto typography = [this, size, tracking, leading, alignment] {
        const QStringList names{"Left", "Center", "Right"};
        updateTypography(size->value(), tracking->value(), leading->value(),
                         names[alignment->currentIndex()]);
    };
    for (auto field : {size, tracking, leading})
        connect(field, &QDoubleSpinBox::valueChanged, this, typography);
    connect(alignment, &QComboBox::currentIndexChanged, this, typography);
    auto color = toolbar_->addAction("Text color…");
    color->setObjectName("inlineTextColor");
    connect(color, &QAction::triggered, this, [this] {
        QGuiApplication::inputMethod()->commit();
        auto value = QColorDialog::getColor(text_->textColor(), this);
        if (!value.isValid())
            return;
        auto cursor = text_->textCursor();
        const bool all = !cursor.hasSelection();
        if (all) {
            cursor.select(QTextCursor::Document);
            base_["red"] = value.redF();
            base_["green"] = value.greenF();
            base_["blue"] = value.blueF();
        }
        QTextCharFormat format;
        format.setForeground(value);
        cursor.mergeCharFormat(format);
        if (all)
            text_->mergeCurrentCharFormat(format);
        if (changed)
            changed();
        text_->setFocus();
    });
    auto mapping = toolbar_->addAction("Font Mapping…");
    mapping->setObjectName("inlineFontMapping");
    connect(mapping, &QAction::triggered, this, &CanvasTextEditor::mappingsDialog);
    auto applyAction = toolbar_->addAction("Apply");
    applyAction->setToolTip("Apply text (Ctrl+Enter)");
    applyAction->setObjectName("inlineTextApply");
    connect(applyAction, &QAction::triggered, this, [this] {
        if (apply)
            apply();
    });
    auto cancelAction = toolbar_->addAction("Cancel");
    cancelAction->setToolTip("Cancel text (Escape)");
    cancelAction->setObjectName("inlineTextCancel");
    connect(cancelAction, &QAction::triggered, this, [this] {
        if (cancel)
            cancel();
    });
    UiLanguage::instance().translateObject(toolbar_, true);
    show();
    toolbar_->show();
    proxy_->setFocus();
    text_->setFocus();
    auto cursor = text_->textCursor();
    cursor.movePosition(QTextCursor::End);
    text_->setTextCursor(cursor);
}
CanvasTextEditor::~CanvasTextEditor() {
    delete toolbar_;
}
QJsonObject CanvasTextEditor::style() const {
    return textStyleFromDocument(*text_->document(), base_);
}
void CanvasTextEditor::hideEditor() {
    hide();
    toolbar_->hide();
}
void CanvasTextEditor::setBoxSize(QSizeF size) {
    base_["boxSize"] = QJsonArray{size.width(), size.height()};
    text_->setLineWrapMode(QTextEdit::WidgetWidth);
}
void CanvasTextEditor::updateTypography(double size, double tracking, double leading,
                                        const QString &alignment) {
    QGuiApplication::inputMethod()->commit();
    base_["fontSize"] = size;
    base_["tracking"] = tracking;
    base_["leading"] = leading;
    base_["alignment"] = alignment;
    auto cursor = text_->textCursor();
    cursor.select(QTextCursor::Document);
    QTextCharFormat format;
    format.setProperty(QTextFormat::FontPixelSize, int(std::lround(size)));
    format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    format.setFontLetterSpacing(tracking);
    cursor.mergeCharFormat(format);
    QTextBlockFormat block;
    block.setAlignment(alignment == "Center"  ? Qt::AlignHCenter
                       : alignment == "Right" ? Qt::AlignRight
                                              : Qt::AlignLeft);
    block.setLineHeight(leading > 0 ? leading : size * 1.2, QTextBlockFormat::FixedHeight);
    cursor.mergeBlockFormat(block);
    text_->mergeCurrentCharFormat(format);
    if (changed)
        changed();
}
QVector<QPointF> CanvasTextEditor::handles() const {
    const double w = sourceSize_.width(), h = sourceSize_.height();
    QVector<QPointF> points{{0, 0}, {w / 2, 0}, {w, 0}, {w, h / 2},
                            {w, h}, {w / 2, h}, {0, h}, {0, h / 2}};
    for (auto &point : points)
        point = placement_.map(point);
    return points;
}
void CanvasTextEditor::place(QTransform map, QSize size) {
    placement_ = map;
    sourceSize_ = size;
    setGeometry(parentWidget()->rect());
    setSceneRect(QRectF(rect()));
    text_->setFixedSize(size);
    proxy_->setTransform(map);
    QPainterPath area;
    area.addPolygon(map.map(QPolygonF(QRectF(QPointF(), size))));
    area.closeSubpath();
    QPainterPathStroker stroke;
    stroke.setWidth(20);
    auto covered = area.united(stroke.createStroke(area));
    setMask(QRegion(covered.toFillPolygon().toPolygon()));
    toolbar_->setGeometry(4, 4, std::max(100, parentWidget()->width() - 8),
                          toolbar_->sizeHint().height());
    toolbar_->raise();
    viewport()->update();
}
void CanvasTextEditor::drawForeground(QPainter *p, const QRectF &) {
    const auto points = handles();
    p->setPen(QPen(QColor(75, 180, 255), 1));
    p->setBrush(Qt::NoBrush);
    p->drawPolygon(QPolygonF{points[0], points[2], points[4], points[6]});
    p->setBrush(Qt::white);
    for (const auto &point : points)
        p->drawRect(QRectF(point - QPointF(3, 3), QSizeF(6, 6)));
}
void CanvasTextEditor::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        const auto points = handles();
        for (int i = 0; i < points.size(); ++i)
            if (QLineF(points[i], event->position()).length() <= 7) {
                handle_ = i;
                break;
            }
        auto local = placement_.inverted().map(event->position());
        if (handle_ == -2 &&
            (local.x() < 4 || local.y() < 4 || local.x() > sourceSize_.width() - 4 ||
             local.y() > sourceSize_.height() - 4))
            handle_ = -1;
        if (handle_ != -2) {
            QGuiApplication::inputMethod()->commit();
            framePlacement_ = placement_;
            frameSize_ = sourceSize_;
            framePress_ = local;
            if (frameStarted)
                frameStarted();
            event->accept();
            return;
        }
    }
    QGraphicsView::mousePressEvent(event);
}
void CanvasTextEditor::mouseMoveEvent(QMouseEvent *event) {
    if (handle_ == -2) {
        QGraphicsView::mouseMoveEvent(event);
        return;
    }
    const auto delta = framePlacement_.inverted().map(event->position()) - framePress_;
    QRectF next(QPointF(), frameSize_);
    if (handle_ == -1) {
        auto movement = delta;
        if (event->modifiers() & Qt::ShiftModifier) {
            if (std::abs(movement.x()) >= std::abs(movement.y()))
                movement.setY(0);
            else
                movement.setX(0);
        }
        next.translate(movement);
    } else {
        if (handle_ == 0 || handle_ == 6 || handle_ == 7)
            next.setLeft(delta.x());
        if (handle_ == 2 || handle_ == 3 || handle_ == 4)
            next.setRight(frameSize_.width() + delta.x());
        if (handle_ <= 2)
            next.setTop(delta.y());
        if (handle_ >= 4 && handle_ <= 6)
            next.setBottom(frameSize_.height() + delta.y());
        if (event->modifiers() & Qt::ShiftModifier) {
            const auto ratio = double(frameSize_.width()) / frameSize_.height();
            double w = std::max(16.0, next.width()), h = std::max(16.0, next.height());
            if (handle_ == 1 || handle_ == 5)
                w = h * ratio;
            else
                h = w / ratio;
            auto anchor =
                QPointF(handle_ == 0 || handle_ == 6 || handle_ == 7 ? frameSize_.width() : 0,
                        handle_ <= 2 ? frameSize_.height() : 0);
            next = QRectF(anchor - QPointF(anchor.x() ? w : 0, anchor.y() ? h : 0), QSizeF(w, h));
        }
    }
    if (next.width() >= 16 && next.height() >= 16 && frameChanged)
        frameChanged(next);
    event->accept();
}
void CanvasTextEditor::mouseReleaseEvent(QMouseEvent *event) {
    if (handle_ != -2) {
        mouseMoveEvent(event);
        handle_ = -2;
        event->accept();
    } else
        QGraphicsView::mouseReleaseEvent(event);
}
void CanvasTextEditor::wheelEvent(QWheelEvent *event) {
    // Scroll the canvas, while the native text input retains its caret and IME focus.
    QWheelEvent forwarded(event->position(), event->globalPosition(), event->pixelDelta(),
                          event->angleDelta(), event->buttons(), event->modifiers(), event->phase(),
                          event->inverted());
    QCoreApplication::sendEvent(parentWidget(), &forwarded);
    event->accept();
}
void CanvasTextEditor::mappingsDialog() {
    QGuiApplication::inputMethod()->commit();
    auto current = style();
    QStringList requested{current.value("fontName").toString()};
    for (const auto &value : current.value("fontRuns").toArray())
        requested << value.toObject().value("fontName").toString();
    requested.removeDuplicates();
    QDialog dialog(this);
    dialog.setObjectName("fontMappingDialog");
    dialog.setWindowTitle("Font Mapping");
    auto layout = new QVBoxLayout(&dialog);
    auto tree = new QTreeWidget;
    tree->setHeaderLabels({uiText("Document font"), uiText("Windows font")});
    layout->addWidget(tree);
    QMap<QString, QFontComboBox *> fields;
    for (const auto &name : requested) {
        auto row = new QTreeWidgetItem(tree, {name});
        auto font = new QFontComboBox;
        font->setCurrentFont(QFont(resolvedTextFont(name)));
        tree->setItemWidget(row, 1, font);
        fields[name] = font;
    }
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                        QDialogButtonBox::Reset);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QAbstractButton::clicked, &dialog, [&] {
        for (auto it = fields.begin(); it != fields.end(); ++it)
            it.value()->setCurrentFont(QFont(resolvedTextFont(it.key(), false)));
    });
    UiLanguage::instance().translateObject(&dialog, true);
    dialog.resize(520, 260);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto mappings = textFontMappings();
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const auto selected = it.value()->currentFont().family();
        if (selected == resolvedTextFont(it.key(), false))
            mappings.remove(it.key());
        else
            mappings[it.key()] = selected;
    }
    setTextFontMappings(mappings);
    const auto position = text_->textCursor().position();
    const auto anchor = text_->textCursor().anchor();
    {
        QSignalBlocker blocker(text_);
        loadTextDocument(*text_->document(), current);
    }
    auto cursor = text_->textCursor();
    const auto end = text_->document()->characterCount() - 1;
    cursor.setPosition(std::min(anchor, end));
    cursor.setPosition(std::min(position, end), QTextCursor::KeepAnchor);
    text_->setTextCursor(cursor);
    auto font = toolbar_->findChild<QFontComboBox *>("inlineTextFont");
    {
        QSignalBlocker blocker(font);
        font->setCurrentFont(QFont(resolvedTextFont(current.value("fontName").toString())));
        font->setToolTip(current.value("fontName").toString() + " → " +
                         font->currentFont().family());
    }
    if (fontsChanged)
        fontsChanged();
    text_->setFocus();
}
} // namespace compositor
