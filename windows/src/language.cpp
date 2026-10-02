// SPDX-License-Identifier: MIT
#include "language.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFontComboBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QWidget>
static void initializeTranslationResources() {
    Q_INIT_RESOURCE(translations);
}
namespace compositor {
namespace {
constexpr int SourceRole = Qt::UserRole + 128;
QJsonObject catalog(const QString &code) {
    QFile file(":/translations/" + code + ".json");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
const QJsonObject &dictionary(const QString &code) {
    static const auto chinese = catalog("zh_CN"), japanese = catalog("ja_JP");
    return code == "ja_JP" ? japanese : chinese;
}
} // namespace
UiLanguage &UiLanguage::instance() {
    static auto manager = new UiLanguage;
    return *manager;
}
UiLanguage::UiLanguage() : QObject(qApp) {
    initializeTranslationResources();
    qApp->installEventFilter(this);
}
void UiLanguage::initialize() {
    auto system = QLocale::system().language();
    QString initial = system == QLocale::Chinese    ? "zh_CN"
                      : system == QLocale::Japanese ? "ja_JP"
                                                    : "en";
    setLanguage(QSettings().value("ui/language", initial).toString(), false);
}
QString UiLanguage::text(const QString &source) const {
    if (code_ == "en" || source.isEmpty())
        return source;
    auto &d = dictionary(code_);
    if (d.contains(source))
        return d.value(source).toString();
    if (source.contains('\n')) {
        auto lines = source.split('\n');
        for (auto &line : lines)
            line = text(line);
        return lines.join('\n');
    }
    for (auto prefix :
         QStringList{"Develop RAW — ", "Layer Effect: ", "Undo ", "Redo ", "Saved ", "Exported "})
        if (source.startsWith(prefix)) {
            QString label = prefix.trimmed();
            label.remove(" —");
            label.remove(':');
            return text(label) + (prefix.contains("—") ? " — " : " ") +
                   ((prefix.startsWith("Develop") || prefix == "Saved " || prefix == "Exported ")
                        ? source.mid(prefix.size())
                        : text(source.mid(prefix.size())));
        }
    // Keep whitespace and menu accelerators outside catalog keys.
    QString plain = source.trimmed();
    plain.remove('&');
    bool ellipsis = plain.endsWith("…");
    if (ellipsis)
        plain.chop(1);
    bool colon = plain.endsWith(':');
    if (colon)
        plain.chop(1);
    if (plain.startsWith("Color ") && plain.mid(6).toInt() > 0)
        return text("Color") + " " + plain.mid(6);
    if (!d.contains(plain))
        return source;
    QString translated = d.value(plain).toString();
    auto mnemonic = source.indexOf('&');
    if (mnemonic >= 0 && mnemonic + 1 < source.size())
        translated += "(&" + source.mid(mnemonic + 1, 1).toUpper() + ")";
    if (ellipsis)
        translated += "…";
    if (colon)
        translated += ':';
    int leading = 0, trailing = 0;
    while (leading < source.size() && source[leading].isSpace())
        ++leading;
    while (trailing < source.size() - leading && source[source.size() - 1 - trailing].isSpace())
        ++trailing;
    return source.left(leading) + translated + source.right(trailing);
}
void UiLanguage::translateObject(QObject *object, bool children) {
    if (changing_ || !object)
        return;
    changing_ = true;
    auto property = [&](const char *name) {
        auto current = object->property(name);
        if (!current.isValid() || current.metaType().id() != QMetaType::QString)
            return;
        QByteArray originalKey = QByteArray("_uiSource_") + name,
                   lastKey = QByteArray("_uiLast_") + name;
        QString value = current.toString(), source;
        if (object->property(lastKey).isValid() && object->property(lastKey).toString() == value)
            source = object->property(originalKey).toString();
        else {
            source = value;
            object->setProperty(originalKey, source);
        }
        auto result = text(source);
        object->setProperty(lastKey, result);
        if (result != value)
            object->setProperty(name, result);
    };
    if (qobject_cast<QWidget *>(object)) {
        property("windowTitle");
        property("toolTip");
        property("statusTip");
        property("placeholderText");
        property("suffix");
        property("prefix");
    }
    if (qobject_cast<QLabel *>(object) || qobject_cast<QAbstractButton *>(object))
        property("text");
    if (auto action = qobject_cast<QAction *>(object)) {
        property("text");
        property("toolTip");
        property("statusTip");
        if (!action->property("_uiBound").toBool()) {
            action->setProperty("_uiBound", true);
            connect(action, &QAction::changed, this, [this, action] { translateObject(action); });
        }
    }
    if (auto combo = qobject_cast<QComboBox *>(object);
        combo && !qobject_cast<QFontComboBox *>(combo)) {
        QSignalBlocker blocker(combo);
        for (int i = 0; i < combo->count(); ++i) {
            if (!combo->itemData(i, SourceRole).isValid())
                combo->setItemData(i, combo->itemText(i), SourceRole);
            combo->setItemText(i, text(combo->itemData(i, SourceRole).toString()));
        }
    }
    changing_ = false;
    if (children)
        for (auto child : object->findChildren<QObject *>())
            translateObject(child);
}
void UiLanguage::setLanguage(const QString &code, bool persist) {
    QString next = code == "zh_CN" || code == "ja_JP" ? code : "en";
    code_ = next;
    if (persist)
        QSettings().setValue("ui/language", code_);
    for (auto widget : QApplication::allWidgets())
        translateObject(widget, true);
    emit languageChanged();
}
bool UiLanguage::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::Show || event->type() == QEvent::Polish)
        translateObject(object, true);
    else if (event->type() == QEvent::Paint && qobject_cast<QLabel *>(object))
        translateObject(object);
    return QObject::eventFilter(object, event);
}
QString uiText(const QString &source) {
    return UiLanguage::instance().text(source);
}
QString comboValue(const QComboBox *combo) {
    auto value = combo->currentData(SourceRole);
    return value.isValid() ? value.toString() : combo->currentText();
}
void selectComboValue(QComboBox *combo, const QString &source) {
    auto i = combo->findData(source, SourceRole);
    combo->setCurrentIndex(i >= 0 ? i : combo->findText(source));
}
} // namespace compositor
