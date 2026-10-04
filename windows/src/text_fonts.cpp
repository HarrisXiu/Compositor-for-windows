// SPDX-License-Identifier: MIT
#include "text_fonts.h"
#include <QFontDatabase>
#include <QSettings>

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
QString resolvedTextFont(const QString &requested, bool useMappings) {
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
    if (auto fallback = available("Segoe UI"); !fallback.isEmpty())
        return fallback;
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}
} // namespace compositor
