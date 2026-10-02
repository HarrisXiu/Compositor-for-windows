// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <atomic>
#include <memory>
namespace compositor {
struct RawSettings {
    double exposure = 0, temperature = 5000, tint = 0, boost = 1;
    double asShotTemperature = 5000, asShotTint = 0;
    void reset() {
        exposure = 0;
        temperature = asShotTemperature;
        tint = asShotTint;
        boost = 1;
    }
};
bool isRawFile(const QString &path);
QString rawFilePatterns();
// One unpacked camera frame; callers can redevelop it without rereading the file.
class RawSource {
  public:
    static std::shared_ptr<RawSource> open(const QString &path,
                                           std::shared_ptr<std::atomic_bool> cancellation = {});
    ~RawSource();
    QSize size() const;
    QString camera() const;
    RawSettings asShot() const;
    QImage develop(const RawSettings &settings, int longEdge = 0);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RawSource();
};
} // namespace compositor
