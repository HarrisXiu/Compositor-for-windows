// SPDX-License-Identifier: MIT
#include "adjustment_tools.h"
#include "document.h"
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <vector>
extern "C" {
#include "LevelsPixels.h"
}

namespace compositor {
namespace {
double number(const QJsonObject &o, const char *key, double fallback = 0) {
    return o.value(QLatin1String(key)).toDouble(fallback);
}
double wrap(double degrees) {
    degrees = std::fmod(degrees, 360);
    return degrees < 0 ? degrees + 360 : degrees;
}
// -180…180: where `degrees` lies from `center` the short way round.
double offset(double degrees, double center) {
    double d = wrap(degrees - center);
    return d > 180 ? d - 360 : d;
}
QJsonObject band(double falloffStart, double rangeStart, double rangeEnd, double falloffEnd) {
    return {{"falloffStart", wrap(falloffStart)},
            {"rangeStart", wrap(rangeStart)},
            {"rangeEnd", wrap(rangeEnd)},
            {"falloffEnd", wrap(falloffEnd)}};
}
// The handles as offsets from the band's middle, so they can be compared in order.
std::array<double, 4> relative(const std::array<double, 4> &handles, double *center) {
    const double span = wrap(handles[3] - handles[0]);
    *center = wrap(handles[0] + span / 2);
    std::array<double, 4> result{};
    for (int i = 0; i < 4; ++i)
        result[i] = offset(handles[i], *center);
    result[0] = -span / 2;
    result[3] = span / 2;
    return result;
}
} // namespace

Histogram levelsHistogram(const QImage &source, const QImage &coverage) {
    Histogram result{};
    if (source.isNull())
        return result;
    const auto image = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QImage mask;
    if (!coverage.isNull()) {
        require(coverage.size() == image.size(), "Selection dimensions differ");
        mask = coverage.convertToFormat(QImage::Format_Grayscale8);
    }
    std::vector<double> bins(1024, 0.0);
    for (int y = 0; y < image.height(); ++y)
        levels_histogram(image.constScanLine(y), mask.isNull() ? nullptr : mask.constScanLine(y),
                         size_t(image.width()), bins.data());
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 256; ++i)
            result[c][i] = bins[size_t(c * 256 + i)];
    return result;
}
double histogramScale(const std::array<double, 256> &bins) {
    double peak = 0;
    std::vector<double> interior;
    for (int i = 0; i < 256; ++i) {
        if (!std::isfinite(bins[i]) || bins[i] <= 0)
            continue;
        peak = std::max(peak, bins[i]);
        if (i > 0 && i < 255)
            interior.push_back(bins[i]);
    }
    if (peak <= 0 || interior.empty())
        return peak;
    std::sort(interior.begin(), interior.end());
    const double typical = interior[size_t(double(interior.size() - 1) * .95)];
    return std::min(peak, typical * 4);
}

QJsonObject identityLevels() {
    return {{"black", 0}, {"white", 255}, {"gamma", 1}, {"outputBlack", 0}, {"outputWhite", 255}};
}
double levelsMap(double value, const QJsonObject &r) {
    const double low = number(r, "black"), high = number(r, "white", 255);
    const double gamma = std::max(.1, number(r, "gamma", 1));
    const double t =
        std::pow(std::clamp((value * 255 - low) / std::max(1.0, high - low), 0.0, 1.0), 1 / gamma);
    return (number(r, "outputBlack") + (number(r, "outputWhite", 255) - number(r, "outputBlack")) * t) /
           255;
}
QJsonArray autoLevels(const Histogram &histogram, AutoLevels mode) {
    QJsonArray result{identityLevels(), identityLevels(), identityLevels(), identityLevels()};
    // The darkest and lightest values, ignoring the 0.1% of the image at either end.
    auto endpoints = [](const std::array<double, 256> &bins, int *low, int *high) {
        double total = 0;
        for (double b : bins)
            total += b;
        if (total <= 0)
            return false;
        double sum = 0;
        *low = 0;
        *high = 255;
        for (int i = 0; i < 256; ++i)
            if ((sum += bins[i]) > total * .001) {
                *low = i;
                break;
            }
        sum = 0;
        for (int i = 255; i >= 0; --i)
            if ((sum += bins[i]) > total * .001) {
                *high = i;
                break;
            }
        return *low < *high;
    };
    if (mode == AutoLevels::Contrast) {
        // One interval for all three keeps the channels' relationships, so colors don't shift.
        int low = 256, high = -1;
        for (int c = 1; c <= 3; ++c) {
            int l, h;
            if (endpoints(histogram[c], &l, &h)) {
                low = std::min(low, l);
                high = std::max(high, h);
            }
        }
        if (low < high) {
            auto range = identityLevels();
            range["black"] = low;
            range["white"] = high;
            result[0] = range;
        }
        return result;
    }
    for (int c = 1; c <= 3; ++c) {
        int low, high;
        if (!endpoints(histogram[c], &low, &high))
            continue;
        auto range = identityLevels();
        range["black"] = low;
        range["white"] = high;
        if (mode == AutoLevels::NeutralMidtones) {
            double total = 0, mean = 0;
            for (int i = 0; i < 256; ++i) {
                total += histogram[c][i];
                mean += levelsMap(i / 255.0, range) * histogram[c][i];
            }
            mean /= total;
            if (mean > 0 && mean < 1)
                range["gamma"] = std::clamp(std::log(mean) / std::log(.5), .1, 9.99);
        }
        result[c] = range;
    }
    return result;
}
QJsonArray sampleLevels(QJsonArray ranges, const QColor &original, LevelsSample mode) {
    while (ranges.size() < 4)
        ranges.append(identityLevels());
    ranges[0] = identityLevels();
    const double rgb[3] = {original.redF() * 255, original.greenF() * 255, original.blueF() * 255};
    for (int c = 1; c <= 3; ++c) {
        auto range = ranges[c].toObject();
        const double v = rgb[c - 1], black = number(range, "black"),
                     white = number(range, "white", 255);
        if (mode == LevelsSample::Black)
            range["black"] = std::clamp(v, 0.0, white - 1);
        else if (mode == LevelsSample::White)
            range["white"] = std::clamp(v, black + 1, 255.0);
        else {
            const double fraction = (v - black) / std::max(1.0, white - black);
            if (fraction <= 0 || fraction >= 1)
                continue;
            range["gamma"] = std::clamp(std::log(fraction) / std::log(.5), .1, 9.99);
        }
        range["outputBlack"] = 0;
        range["outputWhite"] = 255;
        ranges[c] = range;
    }
    return ranges;
}

QStringList hueRanges() {
    return {"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
}
QJsonObject defaultHueBand(const QString &range) {
    const int index = std::max(1, int(hueRanges().indexOf(range)));
    return hueBandAround((index - 1) * 60.0);
}
QJsonObject hueBand(const QJsonObject &settings, const QString &range) {
    const auto stored = settings.value("bands").toObject().value(range).toObject();
    return stored.contains("falloffStart") ? stored : defaultHueBand(range);
}
QJsonObject hueBandAround(double hue) {
    return band(hue - 45, hue - 15, hue + 15, hue + 45);
}
std::array<double, 4> hueHandles(const QJsonObject &b) {
    return {number(b, "falloffStart"), number(b, "rangeStart"), number(b, "rangeEnd"),
            number(b, "falloffEnd")};
}
QJsonObject widenHueBand(QJsonObject b, double hue) {
    auto h = hueHandles(b);
    double center;
    auto o = relative(h, &center);
    const double at = offset(hue, center);
    if (at >= o[1] && at <= o[2])
        return b;
    // The nearer end of the full-strength part reaches out to the hue, its fade going with it.
    if (at > o[2]) {
        o[3] += at - o[2];
        o[2] = at;
    } else {
        o[0] -= o[1] - at;
        o[1] = at;
    }
    if (o[3] - o[0] >= 359)
        return b;
    return band(center + o[0], center + o[1], center + o[2], center + o[3]);
}
QJsonObject narrowHueBand(QJsonObject b, double hue) {
    auto h = hueHandles(b);
    double center;
    auto o = relative(h, &center);
    const double at = offset(hue, center);
    if (at < o[0] || at > o[3])
        return b;
    // The end nearer the hue pulls back past it: the band now fades in (or out) from there.
    if (at - o[1] < o[2] - at) {
        o[1] = std::min(o[2], at + 1);
        o[0] = at;
    } else {
        o[2] = std::max(o[1], at - 1);
        o[3] = at;
    }
    return band(center + o[0], center + o[1], center + o[2], center + o[3]);
}
QString nearestHueRange(double hue) {
    const int index = int(std::lround(wrap(hue) / 60)) % 6;
    return hueRanges()[index + 1];
}
QJsonObject moveHueHandle(QJsonObject b, int index, double degrees) {
    auto h = hueHandles(b);
    double center;
    auto o = relative(h, &center);
    double at = offset(degrees, center);
    if (index == 0)
        at = std::min(at, o[1]);
    else if (index == 3)
        at = std::max(at, o[2]);
    else if (index == 1)
        at = std::clamp(at, o[0], o[2]);
    else
        at = std::clamp(at, o[1], o[3]);
    o[size_t(index)] = at;
    if (o[3] - o[0] >= 360)
        return b;
    return band(center + o[0], center + o[1], center + o[2], center + o[3]);
}
} // namespace compositor
