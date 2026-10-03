// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QJsonObject>
#include <functional>
namespace compositor {
struct EffectImage {
    QImage image;
    int inset = 0;
};
void validateEffects(const QJsonObject &effects);
EffectImage renderEffects(const QImage &shown, const QJsonObject &effects);
// How far past the layer's pixels the effects reach, in layer pixels: EffectImage::inset.
int effectsInset(const QJsonObject &effects);
// How far any effect pixel's value can come from, in layer pixels, in any direction.
int effectsReach(const QJsonObject &effects);
// After the layer's pixels changed within `changed` (layer pixels), redraws the part of `built`
// (from renderEffects) that the change reaches, in place. `shownArea` gives the layer's pixels,
// as renderEffects was given them, within a rectangle that may extend past them (transparent
// there). Returns the rewritten rectangle of built.image.
QRect updateEffects(EffectImage &built, const QJsonObject &effects, const QRect &changed,
                    const std::function<QImage(const QRect &)> &shownArea);
} // namespace compositor
