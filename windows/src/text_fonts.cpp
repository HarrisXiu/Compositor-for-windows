// SPDX-License-Identifier: MIT
#include "text_fonts.h"
#include <QFontDatabase>
#include <QSettings>
#include <cstring>

namespace compositor {
QMap<QString, QString> textFontMappings() {
    QMap<QString, QString> out;
    const auto values = QSettings().value("fonts/substitutions").toMap();
    for (auto it = values.begin(); it != values.end(); ++it)
        out[it.key()] = it.value().toString();
    return out;
}
void setTextFontMappings(const QMap<QString, QString> &mappings) {
    QVariantMap values;
    for (auto it = mappings.begin(); it != mappings.end(); ++it)
        if (!it.key().trimmed().isEmpty() && !it.value().trimmed().isEmpty())
            values[it.key()] = it.value();
    QSettings().setValue("fonts/substitutions", values);
}
QString installedTextFont(const QString &requested, bool useMappings) {
    const auto installed = QFontDatabase::families();
    auto available = [&](const QString &name) {
        for (const auto &family : installed)
            if (family.compare(name, Qt::CaseInsensitive) == 0)
                return family;
        return QString();
    };
    if (useMappings)
        if (auto mapped = available(textFontMappings().value(requested)); !mapped.isEmpty())
            return mapped;
    if (auto exact = available(requested); !exact.isEmpty())
        return exact;
    // Photoshop and Mac documents often carry PostScript names instead of family names.
    const QMap<QString, QString> aliases{{"ArialMT", "Arial"},
                                         {"Helvetica", "Arial"},
                                         {"Helvetica Neue", "Arial"},
                                         {"HelveticaNeue", "Arial"},
                                         {"Times", "Times New Roman"},
                                         {"Times-Roman", "Times New Roman"},
                                         {"TimesNewRomanPSMT", "Times New Roman"},
                                         {"Menlo", "Consolas"},
                                         {"Monaco", "Consolas"},
                                         {"PingFang SC", "Microsoft YaHei"},
                                         {"Hiragino Sans", "Yu Gothic"}};
    auto candidate = aliases.value(requested);
    if (requested.startsWith("SFPro") || requested.startsWith("SF Pro") ||
        requested == "-apple-system")
        candidate = "Segoe UI";
    if (auto alias = available(candidate); !alias.isEmpty())
        return alias;
    // Any other PostScript name ("BodoniMT", "Arial-BoldMT", "TimesNewRomanPSMT"): the installed
    // family it spells, ignoring spaces, hyphens and case, then without its style suffix and
    // Adobe's "PSMT"/"MT"/"PS" endings. The style itself (bold, italic) is not carried over.
    auto squeeze = [](QString name) {
        name.remove(' ');
        name.remove('-');
        name.remove('_');
        return name.toLower();
    };
    QStringList names{requested, requested.section('-', 0, 0)};
    for (const auto &name : QStringList(names))
        for (auto ending : {"PSMT", "MT", "PS"})
            if (name.endsWith(QLatin1String(ending)) && name.size() > int(strlen(ending)))
                names << name.chopped(int(strlen(ending)));
    for (const auto &name : names) {
        const auto key = squeeze(name);
        if (key.size() < 3)
            continue;
        for (const auto &family : installed)
            if (squeeze(family) == key)
                return family;
    }
    return {};
}
QString resolvedTextFont(const QString &requested, bool useMappings) {
    if (auto installed = installedTextFont(requested, useMappings); !installed.isEmpty())
        return installed;
    const auto families = QFontDatabase::families();
    for (const auto &family : families)
        if (family.compare("Segoe UI", Qt::CaseInsensitive) == 0)
            return family;
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}
} // namespace compositor
