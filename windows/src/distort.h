// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QJsonObject>
#include <QVector>

namespace compositor {
// Free distortion (Ctrl-drag a transform handle) and the transform algebra group edits share.
// Layer transforms are affine, so a distortion resamples the pixels into the new shape and leaves
// an ordinary upright layer over the shape's bounds.

// A transform's box corners in document pixels: top-left, top-right, bottom-right, bottom-left.
// Flips don't move them; they only change which pixels land on which corner.
QVector<QPointF> transformCorners(const QJsonObject &transform);
// Four finite corners with area. A convex shape is warped in perspective; one with a corner pulled
// past its neighbors is warped as two triangles instead.
bool distortUsable(const QVector<QPointF> &corners);
bool distortConvex(const QVector<QPointF> &corners);
// `unit` (0…1 across the box) taken by the perspective that maps the box onto `corners`.
QPointF distortMap(const QVector<QPointF> &corners, QPointF unit);
// Where `placement`'s corners land when the perspective taking `by`'s box to `corners` is applied
// around it too: how a mask placed apart from its layer distorts with the layer.
QVector<QPointF> carriedCorners(const QJsonObject &placement, const QJsonObject &by,
                                const QVector<QPointF> &corners);

// The unit square (a 1×1 image) placed on the document, flips included.
QTransform unitPlacement(const QJsonObject &transform);
// A transform like `like` that places the unit square as `map` does; shear, which only uneven
// scaling of something rotated adds, is dropped.
QJsonObject placedLike(const QJsonObject &like, const QTransform &map);
// `placement` carried along as a box moves from `from` to `to`.
QJsonObject followedTransform(const QJsonObject &placement, const QJsonObject &from,
                              const QJsonObject &to);
// The upright box around the transforms' corners, as a transform; empty when there are none.
QJsonObject uprightBox(const QVector<QJsonObject> &transforms);
// Moves a layer's transform to `moved`; a mask that follows it (linked, placed apart) goes along.
void retransformLayer(Layer &layer, const QJsonObject &moved);

struct Warped {
    QImage image;
    QJsonObject transform;
    QRect crop; // Of the full warp, for trimming a mask to match.
};
// `image`, shown through `transform`, resampled so its corners land on `corners`: the pixels over
// the shape's whole-pixel bounds, and an upright transform for them. `limit` caps the longest
// side, for a quick preview that still covers the same bounds.
Warped warpImage(const QImage &image, const QJsonObject &transform,
                 const QVector<QPointF> &corners, bool mask, double limit = 0);
// A full-size warp cropped to its visible pixels, so the layer hugs what is there.
Warped warpTrimmed(const QImage &image, const QJsonObject &transform,
                   const QVector<QPointF> &corners);
// Resamples a layer's pixels, and a mask that moves with it, so its box `transform` takes
// `corners`. With `limit` it makes the quick preview instead and skips trimming. Editable text and
// shape sources no longer describe the pixels, so they are dropped.
void distortLayer(Layer &layer, const QJsonObject &transform, const QVector<QPointF> &corners,
                  double limit = 0);
} // namespace compositor
