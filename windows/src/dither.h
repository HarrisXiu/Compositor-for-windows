// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QStringList>
namespace compositor {
QStringList ditherStyles();
QImage applyDither(const QImage &source, const QJsonObject &settings);
} // namespace compositor
