// SPDX-License-Identifier: MIT
#include "raw_import.h"
#include <QColorSpace>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <libraw/libraw.h>

namespace compositor {
namespace {
const QStringList extensions = {"3fr", "arw", "bay", "cap", "cr2", "cr3", "crw", "dcr", "dcs",
                                "dng", "drf", "erf", "fff", "gpr", "iiq", "k25", "kdc", "mdc",
                                "mef", "mos", "mrw", "nef", "nrw", "orf", "pef", "ptx", "pxn",
                                "raf", "raw", "rwl", "rw2", "rwz", "sr2", "srf", "srw", "x3f"};
void check(int status, const QString &operation) {
    require(status == LIBRAW_SUCCESS,
            operation + ": " + QString::fromUtf8(libraw_strerror(status)));
}
void safeSize(QSize size) {
    require(size.width() > 0 && size.height() > 0 && size.width() <= MaxSide &&
                size.height() <= MaxSide &&
                qint64(size.width()) * size.height() <= MaxSurfacePixels,
            "RAW image exceeds the supported canvas size");
}
// Planckian white point, converted to linear sRGB. Ratios anchor adjustments to the
// actual camera multipliers, so Reset always retains the camera's own white balance.
std::array<double, 3> white(double t) {
    double x = t <= 4000
                   ? -.2661239e9 / (t * t * t) - .2343580e6 / (t * t) + .8776956e3 / t + .179910
                   : -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t) + .2226347e3 / t + .240390;
    double y = t <= 2222 ? -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - .20219683
               : t <= 4000
                   ? -.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - .16748867
                   : 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - .37001483;
    double X = x / y, Z = (1 - x - y) / y;
    return {std::max(.02, 3.2406 * X - 1.5372 - .4986 * Z),
            std::max(.02, -.9689 * X + 1.8758 + .0415 * Z),
            std::max(.02, .0557 * X - .2040 + 1.0570 * Z)};
}
} // namespace
struct RawSource::Impl {
    LibRaw decoder;
    QMutex mutex;
    QSize dimensions;
    QString camera;
    std::array<float, 4> multipliers{};
    RawSettings defaults;
    std::shared_ptr<std::atomic_bool> cancellation;
};
RawSource::RawSource() : impl_(std::make_unique<Impl>()) {}
RawSource::~RawSource() = default;
bool isRawFile(const QString &path) {
    return extensions.contains(QFileInfo(path).suffix().toLower());
}
QString rawFilePatterns() {
    QStringList out;
    for (auto &ext : extensions)
        out << "*." + ext;
    return out.join(' ');
}
std::shared_ptr<RawSource> RawSource::open(const QString &path,
                                           std::shared_ptr<std::atomic_bool> cancellation) {
    auto result = std::shared_ptr<RawSource>(new RawSource);
    auto &d = result->impl_->decoder;
    result->impl_->cancellation = cancellation;
    if (cancellation)
        d.set_progress_handler(
            [](void *context, LibRaw_progress, int, int) {
                return static_cast<std::atomic_bool *>(context)->load() ? 1 : 0;
            },
            cancellation.get());
    auto info = QFileInfo(path);
    require(info.isFile() && info.size() > 0 && info.size() <= 8LL * 1024 * 1024 * 1024,
            "RAW file is missing or exceeds 8 GiB");
    d.imgdata.rawparams.max_raw_memory_mb = 2048;
#ifdef Q_OS_WIN
    check(d.open_file(reinterpret_cast<const wchar_t *>(path.utf16())), "Open RAW");
#else
    check(d.open_file(path.toUtf8().constData()), "Open RAW");
#endif
    auto &s = d.imgdata.sizes;
    QSize size(s.width, s.height);
    if (s.flip & 4)
        size.transpose();
    safeSize(size);
    require(qint64(s.raw_width) * s.raw_height <= MaxSurfacePixels,
            "RAW sensor size exceeds the memory limit");
    result->impl_->dimensions = size;
    result->impl_->camera =
        QString::fromUtf8(d.imgdata.idata.make) + " " + QString::fromUtf8(d.imgdata.idata.model);
    auto &color = d.imgdata.color;
    bool cameraWb = true;
    for (int i = 0; i < 3; ++i)
        cameraWb &= std::isfinite(color.cam_mul[i]) && color.cam_mul[i] > 0;
    for (int i = 0; i < 4; ++i) {
        float v = cameraWb ? color.cam_mul[i] : color.pre_mul[i];
        result->impl_->multipliers[i] =
            std::isfinite(v) && v > 0 ? v : (i == 3 ? result->impl_->multipliers[1] : 1.f);
    }
    check(d.unpack(), "Decode RAW sensor data");
    require(d.error_count() == 0, "RAW sensor data is incomplete or damaged");
    return result;
}
QSize RawSource::size() const {
    return impl_->dimensions;
}
QString RawSource::camera() const {
    return impl_->camera.trimmed();
}
RawSettings RawSource::asShot() const {
    return impl_->defaults;
}
QImage RawSource::develop(const RawSettings &settings, int longEdge) {
    require(std::isfinite(settings.exposure) && settings.exposure >= -3 && settings.exposure <= 3 &&
                std::isfinite(settings.temperature) && settings.temperature >= 2000 &&
                settings.temperature <= 12000 && std::isfinite(settings.tint) &&
                settings.tint >= -150 && settings.tint <= 150 && std::isfinite(settings.boost) &&
                settings.boost >= 0 && settings.boost <= 1,
            "RAW development settings are out of range");
    require(longEdge >= 0 && longEdge <= MaxSide, "Invalid RAW preview size");
    QMutexLocker lock(&impl_->mutex);
    auto &d = impl_->decoder;
    auto &p = d.imgdata.params;
    p.output_color = 1;
    p.output_bps = 16;
    p.gamm[0] = 1;
    p.gamm[1] = 1;
    p.no_auto_bright = 1;
    p.use_camera_wb = 0;
    p.use_auto_wb = 0;
    p.half_size =
        longEdge > 0 && std::max(impl_->dimensions.width(), impl_->dimensions.height()) > longEdge;
    p.user_qual = longEdge ? 0 : 3;
    // Reserve two stops before the color matrix to retain values above display white.
    p.exp_correc = 1;
    p.exp_shift = .25f;
    p.exp_preser = 0;
    p.bright = 1;
    auto shot = white(impl_->defaults.asShotTemperature), target = white(settings.temperature);
    double tintGain = std::exp2((settings.tint - impl_->defaults.asShotTint) / 300);
    double green = shot[1] / target[1] / tintGain;
    for (int c = 0; c < 4; ++c) {
        int rgb = c == 3 ? 1 : c;
        double gain = shot[rgb] / target[rgb];
        if (rgb == 1)
            gain /= tintGain;
        p.user_mul[c] = float(impl_->multipliers[c] * gain / green);
    }
    check(d.dcraw_process(), "Develop RAW");
    int status = 0;
    auto pixels = std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t *)>(
        d.dcraw_make_mem_image(&status), LibRaw::dcraw_clear_mem);
    check(status, "Read developed RAW");
    require(pixels && pixels->type == LIBRAW_IMAGE_BITMAP && pixels->bits == 16 &&
                (pixels->colors == 3 || pixels->colors == 1),
            "RAW decoder returned an unsupported pixel layout");
    QSize size(pixels->width, pixels->height);
    safeSize(size);
    require(quint64(size.width()) * size.height() * pixels->colors * 2 <= pixels->data_size,
            "RAW decoder returned truncated pixels");
    QImage linear(size, QImage::Format_RGBA64);
    require(!linear.isNull(), "Not enough memory for RAW development");
    auto data = reinterpret_cast<const quint16 *>(pixels->data);
    for (int y = 0; y < size.height(); ++y) {
        auto row = reinterpret_cast<QRgba64 *>(linear.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            row[x] = QRgba64::fromRgba64(data[0], data[pixels->colors == 1 ? 0 : 1],
                                         data[pixels->colors == 1 ? 0 : 2], 65535);
            data += pixels->colors;
        }
    }
    pixels.reset();
    d.free_image();
    if (longEdge && std::max(size.width(), size.height()) > longEdge)
        linear = linear.scaled(longEdge, longEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    std::array<uchar, 65536> lut;
    double gain = 4 * std::exp2(settings.exposure);
    for (int i = 0; i < 65536; ++i) {
        double v = std::clamp(i / 65535.0 * gain, 0.0, 1.0);
        double srgb = v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - .055;
        double contrast = srgb * srgb * (3 - 2 * srgb);
        lut[i] = uchar(std::lround(255 * (srgb + (contrast - srgb) * settings.boost * .35)));
    }
    QImage out(linear.size(), QImage::Format_ARGB32_Premultiplied);
    require(!out.isNull(), "Not enough memory for RAW import");
    for (int y = 0; y < out.height(); ++y) {
        auto src = reinterpret_cast<const QRgba64 *>(linear.constScanLine(y));
        auto dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x)
            dst[x] = qRgb(lut[src[x].red()], lut[src[x].green()], lut[src[x].blue()]);
    }
    out.setColorSpace(QColorSpace::SRgb);
    return out;
}
} // namespace compositor
