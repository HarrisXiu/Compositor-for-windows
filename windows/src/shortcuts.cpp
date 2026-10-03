// SPDX-License-Identifier: MIT
#include "shortcuts.h"
#include "language.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace compositor {
namespace {
QKeyCombination normalized(QKeyCombination key) {
    const auto mods = key.keyboardModifiers() & ~Qt::KeypadModifier;
    auto value = key.key();
    if (value == Qt::Key_Enter)
        value = Qt::Key_Return;
    if (value == Qt::Key_BraceLeft)
        value = Qt::Key_BracketLeft;
    if (value == Qt::Key_BraceRight)
        value = Qt::Key_BracketRight;
    if (value == Qt::Key_Plus && (mods & Qt::ShiftModifier))
        value = Qt::Key_Equal;
    if (value == Qt::Key_Underscore)
        value = Qt::Key_Minus;
    return QKeyCombination(mods, value);
}
} // namespace
Shortcuts &Shortcuts::instance() {
    static Shortcuts settings;
    return settings;
}
Shortcuts::Shortcuts() {
    auto add = [this](const QString &id, const QString &title, const QString &key) {
        entries_.push_back({"canvas/" + id, "Canvas", title, QKeySequence(key)});
    };
    add("cancel", "Cancel current canvas operation", "Esc");
    add("apply", "Apply current canvas operation", "Return");
    add("pan", "Temporary Hand tool (hold)", "Space");
    add("cycle", "Cycle tool mode", "Tab");
    add("swap", "Swap foreground/background", "X");
    add("reset", "Reset colors", "D");
    add("sizeDown", "Decrease brush size", "[");
    add("sizeUp", "Increase brush size", "]");
    add("hardnessDown", "Decrease brush hardness", "Shift+[");
    add("hardnessUp", "Increase brush hardness", "Shift+]");
    add("blendDown", "Previous blend mode", "Shift+-");
    add("blendUp", "Next blend mode", "Shift+=");
    add("shape", "Cycle shape kind", "Shift+U");
    add("marquee", "Cycle marquee shape", "Shift+M");
    add("smear", "Cycle Blur / Smudge / Liquify", "Shift+R");
    for (int i = 0; i <= 9; ++i)
        add("opacity" + QString::number(i), "Opacity digit " + QString::number(i),
            QString::number(i));
    for (const auto &direction : QStringList{"Left", "Right", "Up", "Down"}) {
        add("nudge" + direction, "Nudge " + direction + " 1 px", direction);
        add("nudge10" + direction, "Nudge " + direction + " 10 px", "Shift+" + direction);
        add("pixels" + direction, "Move selected pixels " + direction + " 1 px",
            "Ctrl+" + direction);
        add("pixels10" + direction, "Move selected pixels " + direction + " 10 px",
            "Ctrl+Shift+" + direction);
    }
    QSettings settings;
    settings.beginGroup("shortcuts/v1");
    for (const auto &key : settings.childKeys())
        overrides_[key] = QKeySequence(settings.value(key).toString(), QKeySequence::PortableText);
    // IDs include slashes; allKeys also includes nested menu/tool IDs.
    for (const auto &key : settings.allKeys())
        overrides_[key] = QKeySequence(settings.value(key).toString(), QKeySequence::PortableText);
}
QKeySequence Shortcuts::sequence(const ShortcutEntry &entry) const {
    return overrides_.value(entry.id, entry.original);
}
void Shortcuts::registerAction(QAction *action, const QString &id, const QString &group) {
    auto found =
        std::find_if(entries_.cbegin(), entries_.cend(), [&](const auto &e) { return e.id == id; });
    QString title = action->property("layerAction").toString();
    if (title.isEmpty())
        title = action->property("_uiSource_text").toString();
    if (title.isEmpty())
        title = action->text();
    if (found == entries_.cend())
        entries_.push_back({id, group, title, action->shortcut()});
    actions_.erase(std::remove_if(actions_.begin(), actions_.end(),
                                  [](const auto &entry) { return entry.second.isNull(); }),
                   actions_.end());
    actions_.push_back({id, action});
    action->setProperty("shortcutId", id);
    auto entry =
        std::find_if(entries_.cbegin(), entries_.cend(), [&](const auto &e) { return e.id == id; });
    action->setShortcut(sequence(*entry));
}
QKeyCombination Shortcuts::canvasKey(QKeyCombination input) const {
    input = normalized(input);
    for (const auto &entry : entries_)
        if (entry.group == "Canvas" && !sequence(entry).isEmpty() &&
            normalized(sequence(entry)[0]) == input)
            return entry.original[0];
    for (const auto &entry : entries_)
        if (entry.group == "Canvas" && entry.original[0] == input &&
            sequence(entry) != entry.original)
            return QKeyCombination(Qt::Key_unknown);
    return input;
}
QString Shortcuts::validate(const QMap<QString, QKeySequence> &values) const {
    QMap<QString, QString> used;
    for (const auto &entry : entries_) {
        const auto key = values.value(entry.id, entry.original);
        if (key.isEmpty()) {
            if (entry.id == "canvas/cancel")
                return uiText("Cancel must have a shortcut");
            continue;
        }
        if (key.count() != 1)
            return uiText("Choose a single key with optional modifiers");
        const auto combination = key[0];
        if (combination.keyboardModifiers() & Qt::MetaModifier)
            return uiText("Windows key combinations are reserved");
        if (key == QKeySequence("Alt+Tab") || key == QKeySequence("Ctrl+Alt+Delete"))
            return uiText("This shortcut is reserved by Windows");
        const auto name =
            QKeySequence(normalized(combination)).toString(QKeySequence::PortableText);
        if (used.contains(name))
            return uiText("Shortcut conflict") + ": " + key.toString(QKeySequence::NativeText) +
                   " — " + uiText(used[name]) + " / " + uiText(entry.title);
        used[name] = entry.title;
    }
    return {};
}
bool Shortcuts::save(const QMap<QString, QKeySequence> &values) {
    if (!validate(values).isEmpty())
        return false;
    overrides_.clear();
    QSettings settings;
    settings.beginGroup("shortcuts/v1");
    settings.remove("");
    for (const auto &entry : entries_)
        if (values.value(entry.id, entry.original) != entry.original) {
            overrides_[entry.id] = values[entry.id];
            settings.setValue(entry.id, values[entry.id].toString(QKeySequence::PortableText));
        }
    settings.sync();
    for (const auto &[id, action] : actions_)
        if (action) {
            const auto entry = std::find_if(entries_.cbegin(), entries_.cend(),
                                            [&](const auto &e) { return e.id == id; });
            action->setShortcut(sequence(*entry));
        }
    emit changed();
    return true;
}
void Shortcuts::validateLoaded() {
    // Validate after menus and tools register, including conflicts with their defaults.
    if (!validate(overrides_).isEmpty())
        save({});
}
void Shortcuts::showDialog(QWidget *parent) {
    QDialog dialog(parent);
    dialog.setObjectName("keyboardShortcutsDialog");
    dialog.setWindowTitle(uiText("Keyboard Shortcuts"));
    dialog.resize(760, 620);
    auto layout = new QVBoxLayout(&dialog);
    auto search = new QLineEdit;
    search->setPlaceholderText(uiText("Search commands"));
    layout->addWidget(search);
    auto table = new QTableWidget(entries_.size(), 3);
    table->setObjectName("shortcutTable");
    table->setHorizontalHeaderLabels({uiText("Group"), uiText("Command"), uiText("Shortcut")});
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setColumnWidth(0, 110);
    table->setColumnWidth(2, 190);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int i = 0; i < entries_.size(); ++i) {
        table->setItem(i, 0, new QTableWidgetItem(uiText(entries_[i].group)));
        table->setItem(i, 1, new QTableWidgetItem(uiText(entries_[i].title)));
        auto edit = new QKeySequenceEdit(sequence(entries_[i]));
        edit->setMaximumSequenceLength(1);
        table->setCellWidget(i, 2, edit);
    }
    layout->addWidget(table);
    auto message = new QLabel;
    message->setObjectName("shortcutError");
    message->setWordWrap(true);
    layout->addWidget(message);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                        QDialogButtonBox::RestoreDefaults);
    layout->addWidget(buttons);
    connect(search, &QLineEdit::textChanged, &dialog, [table](const QString &text) {
        for (int i = 0; i < table->rowCount(); ++i)
            table->setRowHidden(i,
                                !table->item(i, 1)->text().contains(text, Qt::CaseInsensitive) &&
                                    !table->item(i, 0)->text().contains(text, Qt::CaseInsensitive));
    });
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog,
            [this, table, message] {
                for (int i = 0; i < entries_.size(); ++i)
                    qobject_cast<QKeySequenceEdit *>(table->cellWidget(i, 2))
                        ->setKeySequence(entries_[i].original);
                message->clear();
            });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [this, table, message, &dialog] {
        QMap<QString, QKeySequence> values;
        for (int i = 0; i < entries_.size(); ++i)
            values[entries_[i].id] =
                qobject_cast<QKeySequenceEdit *>(table->cellWidget(i, 2))->keySequence();
        auto error = validate(values);
        if (!error.isEmpty()) {
            message->setText(error);
            return;
        }
        if (save(values))
            dialog.accept();
    });
    dialog.exec();
}
} // namespace compositor
