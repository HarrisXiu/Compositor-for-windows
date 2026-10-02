// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QByteArrayView>
namespace compositor {
struct PhotoshopImport {
    Document document;
    QStringList conversions;
};
bool isPhotoshopFile(const QString &path);
PhotoshopImport readPhotoshop(QByteArrayView bytes, qint64 pixelBudget = documentPixelBudget());
PhotoshopImport importPhotoshop(const QString &path, qint64 pixelBudget = documentPixelBudget());
} // namespace compositor
