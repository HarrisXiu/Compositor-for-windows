// SPDX-License-Identifier: MIT
#include "render.h"
#include "blend.h"
#include "effects.h"
#include "filters.h"
#include <QCache>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QSet>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace compositor {
namespace {
// Where one render draws: the document mapped onto a surface that is part of a full-sized output.
struct View {
    QTransform map; // document → surface
    QSize size;
    QPoint origin;                 // the surface's top-left within the full output
    double scaleX = 1, scaleY = 1; // output pixels per document pixel
    bool halvings = false, parallel = true;
};
View makeView(const Document &d, QSize full, QRect pixels, bool halvings, bool parallel) {
    View v;
    v.scaleX = double(full.width()) / d.size().width();
    v.scaleY = double(full.height()) / d.size().height();
    v.map.scale(v.scaleX, v.scaleY);
    if (!pixels.topLeft().isNull())
        v.map *= QTransform::fromTranslate(-pixels.x(), -pixels.y());
    v.size = pixels.size();
    v.origin = pixels.topLeft();
    v.halvings = halvings;
    v.parallel = parallel;
    return v;
}

// Derived images, keyed by their source's cacheKey, which changes whenever its pixels do.
QMutex cacheMutex;
QCache<std::pair<qint64, int>, QImage> halvingCache(256 * 1024); // KiB
QCache<qint64, QImage> maskCache(128 * 1024);
QCache<QString, EffectImage> effectCache(512 * 1024);
template <typename Key>
QImage cachedImage(QCache<Key, QImage> &cache, const Key &key,
                   const std::function<QImage()> &make) {
    {
        QMutexLocker lock(&cacheMutex);
        if (auto hit = cache.object(key))
            return *hit;
    }
    auto image = make();
    QMutexLocker lock(&cacheMutex);
    cache.insert(key, new QImage(image),
                 int(std::min<qint64>(image.sizeInBytes() / 1024 + 1, INT_MAX)));
    return image;
}
template <typename Key>
void storeCached(QCache<Key, QImage> &cache, const Key &key, const QImage &image) {
    QMutexLocker lock(&cacheMutex);
    cache.insert(key, new QImage(image),
                 int(std::min<qint64>(image.sizeInBytes() / 1024 + 1, INT_MAX)));
}
// Removes an entry and hands it over, so it can be changed in place rather than copied.
template <typename Key> QImage takeCached(QCache<Key, QImage> &cache, const Key &key) {
    QMutexLocker lock(&cacheMutex);
    std::unique_ptr<QImage> taken(cache.take(key));
    return taken ? std::move(*taken) : QImage();
}

QImage alphaImage(const QImage &mask) {
    QImage out(mask.size(), QImage::Format_RGBA8888_Premultiplied);
    require(!out.isNull(), "Not enough memory for mask");
    for (int y = 0; y < out.height(); ++y) {
        auto p = out.scanLine(y);
        auto m = mask.constScanLine(y);
        for (int x = 0; x < out.width(); ++x)
            for (int c = 0; c < 4; ++c)
                p[x * 4 + c] = m[x];
    }
    return out;
}
// A mask as the coverage image it is drawn with, built once per mask rather than once per area.
QImage maskSurface(const QImage &mask) {
    return cachedImage(maskCache, mask.cacheKey(), std::function([&] { return alphaImage(mask); }));
}
bool premultipliedFourByte(QImage::Format format) {
    return format == QImage::Format_RGBA8888_Premultiplied ||
           format == QImage::Format_ARGB32_Premultiplied;
}
// Writes `region` of `out`, a halving of `in`: each pixel the average of the 2×2 it covers. On
// odd sizes the last row or column repeats, so the edge doesn't fade.
void halveRegion(const QImage &in, QImage &out, const QRect &region) {
    for (int y = region.top(); y <= region.bottom(); ++y) {
        auto top = in.constScanLine(2 * y),
             bottom = in.constScanLine(std::min(2 * y + 1, in.height() - 1));
        auto p = out.scanLine(y);
        for (int x = region.left(); x <= region.right(); ++x) {
            const int left = 8 * x, right = 4 * std::min(2 * x + 1, in.width() - 1);
            for (int c = 0; c < 4; ++c)
                p[4 * x + c] = uchar(
                    (top[left + c] + top[right + c] + bottom[left + c] + bottom[right + c] + 2) /
                    4);
        }
    }
}
QImage halve(const QImage &source) {
    QImage in = premultipliedFourByte(source.format())
                    ? source
                    : source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QImage out((in.width() + 1) / 2, (in.height() + 1) / 2, in.format());
    require(!out.isNull(), "Not enough memory for preview");
    halveRegion(in, out, out.rect());
    return out;
}
QImage halved(const QImage &image, int level) {
    if (level <= 0)
        return image;
    return cachedImage(halvingCache, std::pair(image.cacheKey(), level),
                       std::function([&] { return halve(halved(image, level - 1)); }));
}
// Moves the halvings of an image's previous version over to `image`, rewriting only what
// `changed` (its pixels) covers at each level. Stops at the first level that wasn't cached.
void carryHalvings(qint64 previousKey, const QImage &image, QRect changed) {
    if (!premultipliedFourByte(image.format()))
        return;
    QImage below = image;
    for (int level = 1; level <= 30 && !changed.isEmpty(); ++level) {
        auto next = takeCached(halvingCache, std::pair(previousKey, level));
        if (next.size() != QSize((below.width() + 1) / 2, (below.height() + 1) / 2) ||
            next.format() != below.format())
            return;
        changed = QRect(QPoint(changed.left() / 2, changed.top() / 2),
                        QPoint(changed.right() / 2, changed.bottom() / 2))
                      .intersected(next.rect());
        halveRegion(below, next, changed);
        storeCached(halvingCache, std::pair(image.cacheKey(), level), next);
        below = next;
    }
}
// How many times to halve an image drawn through `transform` so it is resampled at more than
// half size; 0 when it is drawn at half size or larger.
int halvingLevel(const QTransform &transform, QSize source) {
    const double scale =
        std::sqrt(std::abs(transform.m11() * transform.m22() - transform.m12() * transform.m21()));
    int level = 0;
    while (level < 30 && scale * double(1 << (level + 1)) <= 1 &&
           (source.width() >> (level + 1)) > 0 && (source.height() >> (level + 1)) > 0)
        ++level;
    return level;
}
// Draws `image` through `transform`; from a halved copy when the view allows and it is shrunk
// below half size, scaled back up by the same factor so it lands in the same place.
void drawSource(QPainter &painter, const QImage &image, const QTransform &transform, const View &v,
                bool smooth) {
    const int level = v.halvings && smooth ? halvingLevel(transform, image.size()) : 0;
    if (level == 0) {
        painter.setTransform(transform);
        painter.drawImage(QPointF(), image);
        return;
    }
    painter.setTransform(QTransform::fromScale(1 << level, 1 << level) * transform);
    painter.drawImage(QPointF(), halved(image, level));
}
bool smoothSampling(const Layer &l) {
    return l.transform().value("sampling") != "Nearest";
}

QImage placed(const Layer &l, const QImage &image, const View &v) {
    QImage surface(v.size, QImage::Format_RGBA8888_Premultiplied);
    require(!surface.isNull(), "Not enough memory for layer preview");
    surface.fill(Qt::transparent);
    QPainter painter(&surface);
    const bool smooth = smoothSampling(l);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    drawSource(painter, image, l.placement(image.size()) * v.map, v, smooth);
    return surface;
}
Layer maskPlacementLayer(const Layer &l) {
    Layer maskLayer = l;
    if (l.metadata.value("maskPlacement").isObject())
        maskLayer.metadata["transform"] = l.metadata.value("maskPlacement");
    return maskLayer;
}
void applyMask(QImage &surface, const Layer &l, const View &v) {
    if (l.mask.isNull() || !l.metadata.value("maskEnabled").toBool(true))
        return;
    auto coverage = placed(maskPlacementLayer(l), maskSurface(l.mask), v);
    QPainter painter(&surface);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(0, 0, coverage);
}
double effectiveOpacity(const Document &d, const Layer &l) {
    double value = l.opacity();
    auto parent = l.parent();
    while (!parent.isEmpty()) {
        auto p = d.find(parent);
        if (!p)
            break;
        value *= p->opacity();
        parent = p->parent();
    }
    return value;
}
QJsonObject knownEffects(const Layer &l) {
    auto effects = l.metadata.value("effects").toObject();
    const auto effectKeys = effects.keys();
    for (const auto &key : effectKeys)
        if (!QStringList{"stroke", "shadow", "colorOverlay", "innerShadow", "outerGlow",
                         "innerGlow"}
                 .contains(key))
            effects.remove(key);
    return effects;
}
// What a layer's effects are cached under, for given versions of its pixels and mask.
QString effectsKey(const Layer &l, const QJsonObject &effects, qint64 image, qint64 mask) {
    auto cacheSettings =
        QJsonObject{{"effects", effects}, {"maskEnabled", l.metadata.value("maskEnabled")}};
    if (l.metadata.value("maskPlacement").isObject()) {
        cacheSettings["transform"] = l.transform();
        cacheSettings["maskPlacement"] = l.metadata.value("maskPlacement");
    }
    return QString::number(image) + ":" + QString::number(mask) + ":" +
           QString::fromUtf8(QJsonDocument(cacheSettings).toJson(QJsonDocument::Compact));
}
bool maskApplies(const Layer &l) {
    return !l.mask.isNull() && l.metadata.value("maskEnabled").toBool(true);
}
// The layer's pixels as its effects see them, its mask applied, within `area` (layer pixels;
// transparent past them).
QImage shownArea(const Layer &l, const QRect &area) {
    QImage shown = area == l.image.rect() ? l.image : l.image.copy(area);
    if (maskApplies(l)) {
        QImage mask(area.size(), QImage::Format_RGBA8888_Premultiplied);
        require(!mask.isNull(), "Not enough memory for effects");
        mask.fill(Qt::transparent);
        Layer placement = maskPlacementLayer(l);
        QPainter p(&mask);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        auto transform =
            placement.placement(l.mask.size()) * l.placement(l.image.size()).inverted();
        if (!area.topLeft().isNull())
            transform *= QTransform::fromTranslate(-area.x(), -area.y());
        p.setTransform(transform);
        p.drawImage(0, 0, maskSurface(l.mask));
        p.end();
        QPainter q(&shown);
        q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        q.drawImage(0, 0, mask);
    }
    return shown;
}
int effectsCost(const EffectImage &built) {
    return int(std::min<qint64>(built.image.sizeInBytes() / 1024 + 1, INT_MAX));
}
// A layer's pixels with its effects, at the layer's own resolution; cached.
EffectImage layerEffects(const Layer &l, const QJsonObject &effects) {
    const auto key = effectsKey(l, effects, l.image.cacheKey(), l.mask.cacheKey());
    {
        QMutexLocker lock(&cacheMutex);
        if (auto hit = effectCache.object(key))
            return *hit;
    }
    auto built = renderEffects(shownArea(l, l.image.rect()), effects);
    QMutexLocker lock(&cacheMutex);
    effectCache.insert(key, new EffectImage(built), effectsCost(built));
    return built;
}
// Moves a layer's cached effects over to its edited pixels or mask, redrawing only what the
// change within `changed` (pixels of what was edited) reaches.
void carryEffects(const Layer &l, bool mask, qint64 previousKey, const QRect &changed) {
    const auto effects = knownEffects(l);
    if (effects.isEmpty() || l.image.isNull())
        return;
    std::unique_ptr<EffectImage> built;
    {
        const auto key = effectsKey(l, effects, mask ? l.image.cacheKey() : previousKey,
                                    mask ? previousKey : l.mask.cacheKey());
        QMutexLocker lock(&cacheMutex);
        built.reset(effectCache.take(key));
    }
    if (!built)
        return;
    QRect area = changed;
    if (mask)
        area = maskApplies(l) ? (maskPlacementLayer(l).placement(l.mask.size()) *
                                 l.placement(l.image.size()).inverted())
                                    .mapRect(QRectF(changed))
                                    .toAlignedRect()
                                    .adjusted(-1, -1, 1, 1)
                                    .intersected(l.image.rect())
                              : QRect();
    if (!area.isEmpty()) {
        const auto builtKey = built->image.cacheKey();
        const auto rewritten =
            updateEffects(*built, effects, area, [&](const QRect &r) { return shownArea(l, r); });
        carryHalvings(builtKey, built->image, rewritten);
    }
    const auto cost = effectsCost(*built);
    QMutexLocker lock(&cacheMutex);
    effectCache.insert(effectsKey(l, effects, l.image.cacheKey(), l.mask.cacheKey()),
                       built.release(), cost);
}
QImage ownSurface(const Layer &l, const View &v) {
    auto effects = knownEffects(l);
    if (effects.isEmpty() || l.image.isNull()) {
        auto surface = placed(l, l.image, v);
        applyMask(surface, l, v);
        return surface;
    }
    auto built = layerEffects(l, effects);
    QImage surface(v.size, QImage::Format_RGBA8888_Premultiplied);
    require(!surface.isNull(), "Not enough memory for effects");
    surface.fill(Qt::transparent);
    QPainter p(&surface);
    const bool smooth = smoothSampling(l);
    p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    QTransform translation;
    translation.translate(-built.inset, -built.inset);
    drawSource(p, built.image, translation * l.placement(l.image.size()) * v.map, v, smooth);
    return surface;
}
void folderMasks(QImage &surface, const Document &d, const Layer &l, const View &v) {
    auto parent = l.parent();
    while (!parent.isEmpty()) {
        auto folder = d.find(parent);
        if (!folder)
            break;
        applyMask(surface, *folder, v);
        parent = folder->parent();
    }
}
void fade(QImage &surface, double opacity) {
    for (int y = 0; y < surface.height(); ++y) {
        auto p = surface.scanLine(y);
        for (int x = 0; x < surface.width() * 4; ++x)
            p[x] = uchar(std::lround(p[x] * opacity));
    }
}
QImage coverage(const Document &d, const Layer &l, const View &v, int depth = 0) {
    require(depth <= 256, "Clipping chain too deep");
    QImage surface;
    if (l.image.isNull()) {
        surface = QImage(v.size, QImage::Format_RGBA8888_Premultiplied);
        require(!surface.isNull(), "Not enough memory for clipping mask");
        surface.fill(Qt::transparent);
    } else
        surface = ownSurface(l, v);
    auto source = l.metadata.value("maskSourceID").toString();
    if (!source.isEmpty())
        if (auto base = d.find(normalizedId(source))) {
            auto mask = coverage(d, *base, v, depth + 1);
            QPainter p(&surface);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(0, 0, mask);
        }
    fade(surface, effectiveOpacity(d, l));
    return surface;
}
QImage opaque(const QImage &source) {
    auto out = source;
    out.detach();
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) {
            auto p = out.scanLine(y) + x * 4;
            int a = p[3];
            for (int c = 0; c < 3; ++c)
                p[c] = a ? uchar(std::min(255, int(p[c]) * 255 / a)) : 0;
            p[3] = 255;
        }
    return out;
}
void restoreAlpha(QImage &result, const QImage &original) {
    for (int y = 0; y < result.height(); ++y)
        for (int x = 0; x < result.width(); ++x) {
            auto p = result.scanLine(y) + x * 4;
            auto q = original.constScanLine(y) + x * 4;
            for (int c = 0; c < 3; ++c)
                p[c] = p[3] ? uchar(std::min(255, int(p[c]) * int(q[3]) / int(p[3]))) : 0;
            p[3] = q[3];
        }
}
// An adjustment's settings at the view's scale. Blurs measure in document pixels; noise and grain
// are seeded by output position, so an area gets exactly the pixels it has in the full output.
QJsonObject adjustmentAtScale(const Layer &l, const View &v) {
    auto a = l.metadata.value("adjustment").toObject();
    if (a.value("kind") == "Gaussian Blur")
        a["blurRadius"] = a.value("blurRadius").toDouble(10) * v.scaleX;
    if (a.value("kind") == "Motion Blur")
        a["motionDistance"] = std::max(1.0, a.value("motionDistance").toDouble(10) * v.scaleX);
    auto settings = adjustmentSettings(a);
    if (a.value("kind") == "Add Noise" || a.value("kind") == "Grain") {
        settings["originX"] = v.origin.x();
        settings["originY"] = v.origin.y();
    }
    return settings;
}
void adjustSurface(QImage &result, const Document &d, const Layer &l, bool folders, const View &v) {
    auto adjusted =
        applyFilter(result, l.metadata.value("adjustment").toObject().value("kind").toString(),
                    adjustmentAtScale(l, v));
    if (l.blend() != "Normal") {
        auto blended = opaque(result);
        composite(blended, opaque(adjusted), parseBlendMode(l.blend()), 1, v.parallel);
        restoreAlpha(blended, result);
        adjusted = blended;
    }
    QImage mask(result.size(), QImage::Format_RGBA8888_Premultiplied);
    require(!mask.isNull(), "Not enough memory for adjustment");
    mask.fill(Qt::white);
    applyMask(mask, l, v);
    if (folders)
        folderMasks(mask, d, l, v);
    double opacity = effectiveOpacity(d, l);
    for (int y = 0; y < result.height(); ++y) {
        auto p = result.scanLine(y);
        auto q = adjusted.constScanLine(y);
        auto m = mask.constScanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            double amount = opacity * m[x * 4 + 3] / 255.0;
            for (int c = 0; c < 4; ++c)
                p[x * 4 + c] =
                    uchar(std::lround(p[x * 4 + c] * (1 - amount) + q[x * 4 + c] * amount));
        }
    }
}

using Visible = QVector<const Layer *>;
Visible visibleLayers(const Document &d) {
    Visible layers;
    std::function<void(const QString &, bool)> visit = [&](const QString &parent, bool visible) {
        for (const auto &l : d.layers) {
            if (l.parent() != parent)
                continue;
            bool shown = visible && l.visible();
            if (l.group()) {
                visit(l.id(), shown);
                continue;
            }
            if (shown)
                layers.push_back(&l);
        }
    };
    visit({}, true);
    return layers;
}
// Where the compositing step starting at `index` ends: a clipping base is composited together
// with the contiguous siblings clipped to it; anything else on its own.
qsizetype stepEnd(const Visible &layers, qsizetype index) {
    const auto &l = *layers[index];
    if (l.metadata.value("adjustment").isObject() || l.image.isNull() ||
        !l.metadata.value("maskSourceID").toString().isEmpty())
        return index + 1;
    qsizetype end = index + 1;
    while (end < layers.size() &&
           normalizedId(layers[end]->metadata.value("maskSourceID").toString()) == l.id() &&
           layers[end]->parent() == l.parent())
        ++end;
    return end;
}
// Composites the steps starting in [begin, end) over `result`.
void composeSteps(const Document &d, const View &v, QImage &result, const Visible &layers,
                  qsizetype begin, qsizetype end) {
    for (qsizetype index = begin, next; index < end; index = next) {
        next = stepEnd(layers, index);
        const auto &l = *layers[index];
        if (l.metadata.value("adjustment").isObject()) {
            if (l.metadata.value("maskSourceID").toString().isEmpty())
                adjustSurface(result, d, l, true, v);
            continue;
        }
        if (l.image.isNull())
            continue;
        auto surface = ownSurface(l, v);
        // Contiguous siblings clipped to one base share its alpha, including soft edges.
        if (next > index + 1) {
            fade(surface, effectiveOpacity(d, l));
            auto stack = opaque(surface);
            for (qsizetype child = index + 1; child < next; ++child) {
                const auto &c = *layers[child];
                if (c.metadata.value("adjustment").isObject())
                    adjustSurface(stack, d, c, false, v);
                else if (!c.image.isNull())
                    composite(stack, ownSurface(c, v), parseBlendMode(c.blend()),
                              effectiveOpacity(d, c), v.parallel);
            }
            restoreAlpha(stack, surface);
            folderMasks(stack, d, l, v);
            composite(result, stack, parseBlendMode(l.blend()), 1, v.parallel);
            continue;
        }
        auto source = l.metadata.value("maskSourceID").toString();
        if (!source.isEmpty())
            if (auto base = d.find(normalizedId(source))) {
                auto mask = coverage(d, *base, v);
                QPainter p(&surface);
                p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                p.drawImage(0, 0, mask);
            }
        folderMasks(surface, d, l, v);
        composite(result, surface, parseBlendMode(l.blend()), effectiveOpacity(d, l), v.parallel);
    }
}
// The first step whose pixels depend on `layerId`: it, the layers inside it, and those clipped to
// any of them. Everything before it is unaffected by edits to that layer.
qsizetype splitStep(const Document &d, const Visible &layers, const QString &layerId) {
    QSet<QString> affected{normalizedId(layerId)};
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto &l : d.layers) {
            if (affected.contains(l.id()))
                continue;
            auto source = l.metadata.value("maskSourceID").toString();
            if (affected.contains(l.parent()) ||
                (!source.isEmpty() && affected.contains(normalizedId(source)))) {
                affected.insert(l.id());
                grew = true;
            }
        }
    }
    for (qsizetype index = 0, next; index < layers.size(); index = next) {
        next = stepEnd(layers, index);
        for (auto i = index; i < next; ++i)
            if (affected.contains(layers[i]->id()))
                return index;
    }
    return layers.size();
}
void checkArea(const Document &d, const RenderArea &a) {
    require(!d.size().isEmpty() && !a.full.isEmpty() && !a.pixels.isEmpty() &&
                QRect(QPoint(), a.full).contains(a.pixels),
            "Invalid render area");
}
// The area with room for the blurs' reach, within the output.
QRect marginPixels(const Document &d, const RenderArea &a) {
    const int reach = renderReach(d, a.full);
    return a.pixels.adjusted(-reach, -reach, reach, reach).intersected(QRect(QPoint(), a.full));
}
QImage blankSurface(QSize size) {
    require(qint64(size.width()) * size.height() <= MaxSurfacePixels,
            "Render exceeds 200 megapixel limit");
    QImage result(size, QImage::Format_RGBA8888_Premultiplied);
    require(!result.isNull(), "Not enough memory for canvas");
    result.fill(Qt::transparent);
    return result;
}
QImage crop(const QImage &image, QRect from, QRect to) {
    return from == to ? image : image.copy(to.translated(-from.topLeft()));
}
} // namespace

QImage renderDocument(const Document &d, QSize output) {
    if (output.isEmpty())
        output = d.size();
    require(output.width() > 0 && output.height() > 0 &&
                qint64(output.width()) * output.height() <= MaxSurfacePixels,
            "Render exceeds 200 megapixel limit");
    return renderArea(d, {output, QRect(QPoint(), output)});
}
QImage renderArea(const Document &d, const RenderArea &a) {
    checkArea(d, a);
    const auto pixels = marginPixels(d, a);
    const auto v = makeView(d, a.full, pixels, a.halvings, a.parallel);
    const auto layers = visibleLayers(d);
    auto result = blankSurface(pixels.size());
    composeSteps(d, v, result, layers, 0, layers.size());
    return crop(result, pixels, a.pixels);
}
QImage renderBackdrop(const Document &d, const RenderArea &a, const QString &layerId) {
    checkArea(d, a);
    const auto pixels = marginPixels(d, a);
    const auto v = makeView(d, a.full, pixels, a.halvings, a.parallel);
    const auto layers = visibleLayers(d);
    auto result = blankSurface(pixels.size());
    composeSteps(d, v, result, layers, 0, splitStep(d, layers, layerId));
    return result;
}
QImage bakeClipping(const Document &d, const Layer &l) {
    const auto source = d.find(normalizedId(l.metadata.value("maskSourceID").toString()));
    if (!source || l.image.isNull())
        return l.image;
    auto coverageImage =
        coverage(d, *source, makeView(d, d.size(), QRect(QPoint(), d.size()), false, true));
    QImage mask(l.image.size(), QImage::Format_RGBA8888_Premultiplied);
    require(!mask.isNull(), "Not enough memory for clipping");
    mask.fill(Qt::transparent);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setTransform(l.placement(l.image.size()).inverted());
    painter.drawImage(0, 0, coverageImage);
    painter.end();
    auto result = l.image;
    QPainter apply(&result);
    apply.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    apply.drawImage(0, 0, mask);
    return result;
}
QImage renderOver(const Document &d, const RenderArea &a, const QString &layerId,
                  const QImage &backdrop) {
    checkArea(d, a);
    const auto pixels = marginPixels(d, a);
    require(backdrop.size() == pixels.size() &&
                backdrop.format() == QImage::Format_RGBA8888_Premultiplied,
            "Backdrop does not match the render area");
    const auto v = makeView(d, a.full, pixels, a.halvings, a.parallel);
    const auto layers = visibleLayers(d);
    auto result = backdrop;
    composeSteps(d, v, result, layers, splitStep(d, layers, layerId), layers.size());
    return crop(result, pixels, a.pixels);
}
int renderReach(const Document &d, QSize full) {
    if (d.size().isEmpty())
        return 0;
    const double scale = double(full.width()) / d.size().width();
    qint64 reach = 0;
    for (const auto *l : visibleLayers(d)) {
        const auto a = l->metadata.value("adjustment").toObject();
        if (a.value("kind") == "Gaussian Blur")
            reach +=
                qint64(std::ceil(3 * std::max(0.0, a.value("blurRadius").toDouble(10) * scale)));
        else if (a.value("kind") == "Motion Blur")
            reach += qint64(std::ceil(
                         std::max(1.0, a.value("motionDistance").toDouble(10) * scale) / 2)) +
                     2;
    }
    return int(std::min<qint64>(reach, std::max(full.width(), full.height())));
}
void prepareRender(const Document &d, QSize full, bool halvings) {
    if (d.size().isEmpty() || full.isEmpty())
        return;
    const auto v = makeView(d, full, QRect(QPoint(), full), halvings, true);
    auto warm = [&](const QImage &image, const QTransform &transform, bool smooth) {
        if (halvings && smooth)
            halved(image, halvingLevel(transform, image.size()));
    };
    QSet<QString> folders;
    for (const auto *l : visibleLayers(d)) {
        for (auto parent = l->parent(); !parent.isEmpty();) {
            auto folder = d.find(parent);
            if (!folder || folders.contains(parent))
                break;
            folders.insert(parent);
            parent = folder->parent();
        }
        const bool smooth = smoothSampling(*l);
        auto effects = knownEffects(*l);
        if (!l->image.isNull() && !effects.isEmpty()) {
            auto built = layerEffects(*l, effects);
            warm(built.image,
                 QTransform::fromTranslate(-built.inset, -built.inset) *
                     l->placement(l->image.size()) * v.map,
                 smooth);
        } else if (!l->image.isNull())
            warm(l->image, l->placement(l->image.size()) * v.map, smooth);
        if (!l->mask.isNull()) {
            auto maskLayer = maskPlacementLayer(*l);
            warm(maskSurface(l->mask), maskLayer.placement(l->mask.size()) * v.map,
                 smoothSampling(maskLayer));
        }
    }
    for (const auto &id : folders)
        if (auto folder = d.find(id); folder && !folder->mask.isNull()) {
            auto maskLayer = maskPlacementLayer(*folder);
            warm(maskSurface(folder->mask), maskLayer.placement(folder->mask.size()) * v.map,
                 smoothSampling(maskLayer));
        }
}
void carryRenderCaches(const Layer &layer, bool mask, qint64 previousKey, const QRect &changed) {
    const auto &image = mask ? layer.mask : layer.image;
    const auto region = changed.intersected(image.rect());
    if (region.isEmpty() || previousKey == image.cacheKey())
        return;
    if (!mask)
        carryHalvings(previousKey, image, region);
    else if (auto surface = takeCached(maskCache, previousKey);
             surface.size() == image.size() && image.format() == QImage::Format_Grayscale8) {
        const auto surfaceKey = surface.cacheKey();
        for (int y = region.top(); y <= region.bottom(); ++y) {
            auto p = surface.scanLine(y);
            auto m = image.constScanLine(y);
            for (int x = region.left(); x <= region.right(); ++x)
                for (int c = 0; c < 4; ++c)
                    p[x * 4 + c] = m[x];
        }
        storeCached(maskCache, image.cacheKey(), surface);
        carryHalvings(surfaceKey, surface, region);
    }
    carryEffects(layer, mask, previousKey, region);
}
QRectF layerExtent(const Layer &l) {
    if (l.group() || l.image.isNull() || l.metadata.value("adjustment").isObject())
        return {};
    QRectF bounds(QPointF(), l.image.size());
    if (auto effects = knownEffects(l); !effects.isEmpty()) {
        const int inset = effectsInset(effects);
        bounds.adjust(-inset, -inset, inset, inset);
    }
    return l.placement(l.image.size()).mapRect(bounds);
}
QImage layerSelection(const Document &d, const Layer &l, const QImage &selection) {
    if (selection.isNull())
        return {};
    QImage out(l.image.size(), QImage::Format_Grayscale8);
    require(!out.isNull(), "Not enough memory for selection");
    out.fill(0);
    auto transform = l.placement(l.image.size());
    for (int y = 0; y < out.height(); ++y) {
        auto p = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            auto q = transform.map(QPointF(x + 0.5, y + 0.5));
            int sx = int(std::floor(q.x())), sy = int(std::floor(q.y()));
            if (sx >= 0 && sy >= 0 && sx < d.size().width() && sy < d.size().height())
                p[x] = selection.constScanLine(sy)[sx];
        }
    }
    return out;
}
} // namespace compositor
