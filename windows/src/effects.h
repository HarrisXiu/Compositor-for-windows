// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QJsonObject>
namespace compositor {
struct EffectImage {
    QImage image;
    int inset = 0;
};
void validateEffects(const QJsonObject &effects);
EffectImage renderEffects(const QImage &shown, const QJsonObject &effects);
} // namespace compositor
