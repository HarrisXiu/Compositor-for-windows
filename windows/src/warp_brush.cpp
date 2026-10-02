// SPDX-License-Identifier: MIT
#include "warp_brush.h"
#include "filters.h"
#include <QColorSpace>
#include <QLineF>
#include <QPainter>
#include <algorithm>
#include <cmath>
namespace compositor {
WarpBrush::WarpBrush(const QImage &image, const QTransform &placement, QSize canvas, WarpMode mode,
                     double diameter, double hardness, double strength)
    : mode_(mode), diameter_(std::clamp(diameter, 2.0, 2000.0)),
      hardness_(std::clamp(hardness, 0.0, .98)), strength_(std::clamp(strength, 0.0, 1.0)) {
    require(!image.isNull() && canvas.width() > 0 && canvas.height() > 0 &&
                qint64(canvas.width()) * canvas.height() <= MaxSurfacePixels,
            "Invalid warp canvas");
    require(std::isfinite(diameter) && std::isfinite(hardness) && std::isfinite(strength),
            "Invalid warp brush settings");
    pixels_ = QImage(canvas, QImage::Format_RGBA8888_Premultiplied);
    coverage_ = QImage(canvas, QImage::Format_Grayscale8);
    require(!pixels_.isNull() && !coverage_.isNull(), "Not enough memory for warp brush");
    pixels_.fill(Qt::transparent);
    coverage_.fill(0);
    QPainter p(&pixels_);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setTransform(placement);
    p.drawImage(QPointF(), image);
}
int WarpBrush::radius() const {
    return int(std::ceil(diameter_ / 2));
}
double WarpBrush::weight(double distance) const {
    if (distance >= 1)
        return 0;
    if (distance <= hardness_)
        return 1;
    double t = (1 - distance) / (1 - hardness_);
    return t * t * (3 - 2 * t);
}
bool WarpBrush::append(QPointF point) {
    require(std::isfinite(point.x()) && std::isfinite(point.y()), "Invalid warp stroke point");
    require(std::abs(point.x()) <= 1000000 && std::abs(point.y()) <= 1000000,
            "Warp point exceeds the supported range");
    if (strength_ == 0)
        return false;
    if (!started_) {
        last_ = point;
        started_ = true;
        if (mode_ == WarpMode::Smudge)
            pickup(point);
        return false;
    }
    double distance = QLineF(last_, point).length(),
           spacing = std::max(1.0, diameter_ * (mode_ == WarpMode::Smudge ? .005 : .025));
    if (distance < spacing)
        return false;
    int steps = int(std::ceil(distance / spacing));
    require(steps <= 100000, "Warp stroke exceeds the supported distance");
    auto from = last_, previous = from;
    QPainter coverage(&coverage_);
    coverage.setRenderHint(QPainter::Antialiasing);
    coverage.setPen(Qt::NoPen);
    coverage.setBrush(Qt::white);
    for (int step = 1; step <= steps; ++step) {
        auto next = from + (point - from) * (double(step) / steps);
        if (mode_ == WarpMode::Smudge)
            smudge(next);
        else
            push(previous, next);
        double r = diameter_ / 2 + 2;
        coverage.drawEllipse(next, r, r);
        previous = next;
    }
    last_ = point;
    return true;
}
void WarpBrush::pickup(QPointF center) {
    int r = radius(), side = 2 * r + 1, cx = int(std::lround(center.x())),
        cy = int(std::lround(center.y()));
    carried_.assign(size_t(side) * side * 4, 0);
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            int x = cx + dx, y = cy + dy;
            if (!pixels_.valid(x, y))
                continue;
            auto p = pixels_.constScanLine(y) + x * 4;
            size_t c = (size_t(dy + r) * side + dx + r) * 4;
            for (int k = 0; k < 4; ++k)
                carried_[c + k] = p[k];
        }
}
void WarpBrush::smudge(QPointF center) {
    int r = radius(), side = 2 * r + 1, cx = int(std::lround(center.x())),
        cy = int(std::lround(center.y()));
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            int x = cx + dx, y = cy + dy;
            if (!pixels_.valid(x, y))
                continue;
            double w = weight(std::hypot(dx, dy) / (diameter_ / 2));
            if (!w)
                continue;
            auto p = pixels_.scanLine(y) + x * 4;
            size_t c = (size_t(dy + r) * side + dx + r) * 4;
            for (int k = 0; k < 4; ++k) {
                float painted = float(p[k] + (carried_[c + k] - p[k]) * w * strength_);
                p[k] = uchar(std::clamp(std::lround(painted), 0L, 255L));
                carried_[c + k] = painted;
            }
        }
}
void WarpBrush::push(QPointF from, QPointF to) {
    int r = radius(), cx = int(std::lround(to.x())), cy = int(std::lround(to.y()));
    auto move = (to - from) * strength_;
    int margin = int(std::ceil(std::max(std::abs(move.x()), std::abs(move.y())))) + 2;
    int x0 = std::max(0, cx - r - margin), x1 = std::min(pixels_.width() - 1, cx + r + margin),
        y0 = std::max(0, cy - r - margin), y1 = std::min(pixels_.height() - 1, cy + r + margin);
    if (x0 > x1 || y0 > y1)
        return;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    scratch_.resize(size_t(cw) * ch * 4);
    for (int y = 0; y < ch; ++y) {
        auto p = pixels_.constScanLine(y + y0) + x0 * 4;
        for (int x = 0; x < cw * 4; ++x)
            scratch_[size_t(y) * cw * 4 + x] = p[x];
    }
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            int x = cx + dx, y = cy + dy;
            if (x < x0 || x > x1 || y < y0 || y > y1)
                continue;
            double w = weight(std::hypot(dx, dy) / (diameter_ / 2));
            if (!w)
                continue;
            double sx = std::clamp(x - x0 - move.x() * w, 0.0, double(cw - 1)),
                   sy = std::clamp(y - y0 - move.y() * w, 0.0, double(ch - 1));
            int ix = std::min(cw - 1, int(sx)), iy = std::min(ch - 1, int(sy)),
                nx = std::min(cw - 1, ix + 1), ny = std::min(ch - 1, iy + 1);
            double fx = sx - ix, fy = sy - iy;
            auto p = pixels_.scanLine(y) + x * 4;
            for (int k = 0; k < 4; ++k) {
                auto at = [&](int xx, int yy) { return scratch_[(size_t(yy) * cw + xx) * 4 + k]; };
                double top = at(ix, iy) + (at(nx, iy) - at(ix, iy)) * fx,
                       bottom = at(ix, ny) + (at(nx, ny) - at(ix, ny)) * fx;
                p[k] = uchar(std::clamp(std::lround(top + (bottom - top) * fy), 0L, 255L));
            }
        }
}
QImage WarpBrush::result(const QImage &original, const QTransform &placement,
                         const QImage &selection) const {
    bool invertible = false;
    auto inverse = placement.inverted(&invertible);
    require(invertible, "Layer transform cannot be painted");
    QImage warped(original.size(), QImage::Format_RGBA8888_Premultiplied),
        mask(original.size(), QImage::Format_Grayscale8);
    require(!warped.isNull() && !mask.isNull(), "Not enough memory to commit warp stroke");
    warped.fill(Qt::transparent);
    mask.fill(0);
    QPainter p(&warped);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setTransform(inverse);
    p.drawImage(QPointF(), pixels_);
    p.end();
    QPainter m(&mask);
    m.setRenderHint(QPainter::SmoothPixmapTransform);
    m.setTransform(inverse);
    m.drawImage(QPointF(), coverage_);
    m.end();
    if (!selection.isNull()) {
        require(selection.size() == mask.size(), "Invalid warp selection");
        for (int y = 0; y < mask.height(); ++y)
            for (int x = 0; x < mask.width(); ++x)
                mask.scanLine(y)[x] =
                    uchar((mask.constScanLine(y)[x] * selection.constScanLine(y)[x] + 127) / 255);
    }
    warped.setColorSpace(original.colorSpace());
    return limitToSelection(original, warped, mask);
}
} // namespace compositor
