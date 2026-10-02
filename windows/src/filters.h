// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QString>

namespace compositor {
QImage applyFilter(const QImage &source, const QString &kind, const QJsonObject &settings = {});
QImage gaussianBlur(const QImage &source, double sigma, bool clampEdges = false);
QImage applyAdjustment(const QImage &source, const QJsonObject &adjustment);
QJsonObject adjustmentSettings(const QJsonObject &adjustment);
QJsonObject makeAdjustment(const QString &kind, const QJsonObject &settings);
QImage limitToSelection(const QImage &original, const QImage &filtered, const QImage &coverage);
} // namespace compositor
