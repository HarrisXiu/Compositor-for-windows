// SPDX-License-Identifier: MIT
#pragma once
#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <array>

namespace compositor {
// Levels, Curves and Hue/Saturation helpers behind their panels (F2): histograms, the Auto
// buttons, eyedropper sampling and Hue/Saturation's color bands. Levels ranges are JSON objects
// with black, white, gamma, outputBlack and outputWhite; there are four: RGB, red, green, blue.

// Per channel, how much of the image has each value: RGB (the mean of the three channels), red,
// green, blue. Weighted by alpha and by `coverage` (8-bit, the image's size) when given.
using Histogram = std::array<std::array<double, 256>, 4>;
Histogram levelsHistogram(const QImage &image, const QImage &coverage = {});
// The height a histogram's bars are drawn against: its peak, unless one spike towers over the rest.
double histogramScale(const std::array<double, 256> &bins);

QJsonObject identityLevels();
// The output (0…1) of input `value` (0…1) through one range.
double levelsMap(double value, const QJsonObject &range);
enum class AutoLevels { Contrast, Color, NeutralMidtones };
// Photoshop's Auto buttons: Contrast stretches all three channels together, Color each on its
// own, and Color + neutral midtones also brings each channel's mean to the middle.
QJsonArray autoLevels(const Histogram &histogram, AutoLevels mode);
enum class LevelsSample { Black, Gray, White };
// Sets the per-channel ranges so `original` (the clicked pixel, before any adjustment) becomes
// black, neutral gray or white; the RGB range is reset.
QJsonArray sampleLevels(QJsonArray ranges, const QColor &original, LevelsSample mode);

// Hue/Saturation's color ranges: Master and the six bands, red first.
QStringList hueRanges();
// A band's four handles in degrees: where it fades in, where it is full, where it starts fading
// and where it has faded out. Stored as falloffStart, rangeStart, rangeEnd, falloffEnd.
QJsonObject defaultHueBand(const QString &range);
QJsonObject hueBand(const QJsonObject &settings, const QString &range);
QJsonObject hueBandAround(double hue);
// The band grown to take `hue` in at full strength, or shrunk to leave it out.
QJsonObject widenHueBand(QJsonObject band, double hue);
QJsonObject narrowHueBand(QJsonObject band, double hue);
// The color range whose band centers nearest to `hue`.
QString nearestHueRange(double hue);
// Moves handle `index` (0…3) of a band to `degrees`, keeping the four in order around the circle.
QJsonObject moveHueHandle(QJsonObject band, int index, double degrees);
std::array<double, 4> hueHandles(const QJsonObject &band);
} // namespace compositor
