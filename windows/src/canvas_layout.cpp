// SPDX-License-Identifier: MIT
#include "canvas_layout.h"
#include <QJsonArray>
#include <QSettings>
#include <algorithm>
#include <cmath>
#include <optional>
namespace compositor {
CanvasViewOptions loadCanvasViewOptions() {
    CanvasViewOptions o;
    QSettings s;
    s.beginGroup("canvasView");
    o.rulers = s.value("rulers", o.rulers).toBool();
    o.guides = s.value("guides", o.guides).toBool();
    o.grid = s.value("grid", o.grid).toBool();
    o.snap = s.value("snap", o.snap).toBool();
    o.lockGuides = s.value("lockGuides", o.lockGuides).toBool();
    o.snapCanvas = s.value("snapCanvas", o.snapCanvas).toBool();
    o.snapLayers = s.value("snapLayers", o.snapLayers).toBool();
    o.snapGuides = s.value("snapGuides", o.snapGuides).toBool();
    o.snapGrid = s.value("snapGrid", o.snapGrid).toBool();
    o.transformControls = s.value("transformControls", o.transformControls).toBool();
    o.gridSpacing = std::clamp(s.value("gridSpacing", 64).toInt(), 2, 4096);
    o.gridSubdivisions =
        std::clamp(s.value("gridSubdivisions", 8).toInt(), 1, std::min(64, o.gridSpacing));
    o.guideColor = s.value("guideColor", o.guideColor).value<QColor>();
    o.gridColor = s.value("gridColor", o.gridColor).value<QColor>();
    return o;
}
void saveCanvasViewOptions(const CanvasViewOptions &o) {
    QSettings s;
    s.beginGroup("canvasView");
    s.setValue("rulers", o.rulers);
    s.setValue("guides", o.guides);
    s.setValue("grid", o.grid);
    s.setValue("snap", o.snap);
    s.setValue("lockGuides", o.lockGuides);
    s.setValue("transformControls", o.transformControls);
    s.setValue("snapCanvas", o.snapCanvas);
    s.setValue("snapLayers", o.snapLayers);
    s.setValue("snapGuides", o.snapGuides);
    s.setValue("snapGrid", o.snapGrid);
    s.setValue("gridSpacing", o.gridSpacing);
    s.setValue("gridSubdivisions", o.gridSubdivisions);
    s.setValue("guideColor", o.guideColor);
    s.setValue("gridColor", o.gridColor);
}
namespace {
std::optional<double> snappedCoordinate(const Document &d, const CanvasViewOptions &o, double value,
                                        bool horizontal, double zoom,
                                        const QSet<QString> &exclude) {
    if (!o.snap)
        return {};
    QVector<double> targets;
    if (o.snapCanvas) {
        const double side = horizontal ? d.size().height() : d.size().width();
        targets << 0 << side / 2 << side;
    }
    if (o.snapGuides && o.guides)
        for (auto v : d.metadata.value("guides").toArray()) {
            auto g = v.toObject();
            if ((g.value("axis") == "horizontal") == horizontal)
                targets << g.value("position").toDouble();
        }
    if (o.snapLayers)
        for (const auto &l : d.layers) {
            if (l.group() || !l.visible() || exclude.contains(l.id()))
                continue;
            bool visible = true;
            for (auto parent = d.find(l.parent()); parent; parent = d.find(parent->parent()))
                visible &= parent->visible();
            if (!visible)
                continue;
            auto source = l.image.isNull() ? QSize(1, 1) : l.image.size();
            auto b = l.placement(source).mapRect(QRectF(QPointF(), source));
            targets << (horizontal ? b.top() : b.left())
                    << (horizontal ? b.center().y() : b.center().x())
                    << (horizontal ? b.bottom() : b.right());
        }
    if (o.snapGrid && o.grid) {
        const double step = double(o.gridSpacing) / o.gridSubdivisions;
        targets << std::round(value / step) * step;
    }
    std::optional<double> result;
    double distance = 6 / std::max(.001, zoom);
    for (double t : targets)
        if (std::abs(t - value) < distance) {
            result = t;
            distance = std::abs(t - value);
        }
    return result;
}
} // namespace
double snapCoordinate(const Document &d, const CanvasViewOptions &o, double value, bool horizontal,
                      double zoom, const QSet<QString> &exclude) {
    return snappedCoordinate(d, o, value, horizontal, zoom, exclude).value_or(value);
}
QPointF snapBounds(const Document &d, const CanvasViewOptions &o, const QRectF &b, double zoom,
                   const QSet<QString> &exclude) {
    auto delta = [&](bool horizontal) {
        double result = 0, distance = 6 / std::max(.001, zoom);
        for (double v :
             {horizontal ? b.top() : b.left(), horizontal ? b.center().y() : b.center().x(),
              horizontal ? b.bottom() : b.right()}) {
            const auto target = snappedCoordinate(d, o, v, horizontal, zoom, exclude);
            if (!target)
                continue;
            double change = *target - v;
            if (std::abs(change) < distance) {
                result = change;
                distance = std::abs(change);
            }
        }
        return result;
    };
    return {delta(false), delta(true)};
}
} // namespace compositor
