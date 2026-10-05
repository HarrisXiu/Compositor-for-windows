// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QString>
namespace compositor {
bool isHeifFile(const QString &path);
QImage importWicImage(const QString &path);
} // namespace compositor
