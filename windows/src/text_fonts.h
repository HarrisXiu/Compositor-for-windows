// SPDX-License-Identifier: MIT
#pragma once
#include <QMap>
#include <QString>

namespace compositor {
QString resolvedTextFont(const QString &requested, bool useMappings = true);
// The installed family a requested name stands for (a family, a known alias or a PostScript name
// such as "BodoniMT"), or an empty string when only a fallback font could be used.
QString installedTextFont(const QString &requested, bool useMappings = true);
QMap<QString, QString> textFontMappings();
void setTextFontMappings(const QMap<QString, QString> &mappings);
constexpr int RequestedFontProperty = 0x100000 + 624;
} // namespace compositor
