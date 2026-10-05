// SPDX-License-Identifier: MIT
#include "wic_import.h"
#include "document.h"
#include <QColorSpace>
#include <QDir>
#include <QFileInfo>
#include <QTransform>
#include <cmath>
#ifdef Q_OS_WIN
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace compositor {
bool isHeifFile(const QString &path) {
    return QStringList{"heic", "heif", "hif"}.contains(QFileInfo(path).suffix().toLower());
}
QImage importWicImage(const QString &path) {
#ifdef Q_OS_WIN
    using Microsoft::WRL::ComPtr;
    auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    require(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "Cannot initialize WIC");
    struct Apartment {
        bool owned;
        ~Apartment() {
            if (owned)
                CoUninitialize();
        }
    } apartment{SUCCEEDED(initialized)};
    auto check = [](HRESULT hr, const QString &operation) {
        require(SUCCEEDED(hr), operation + " (WIC 0x" + QString::number(quint32(hr), 16) + ")");
    };
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(factory.GetAddressOf())),
          "Cannot create WIC decoder");
    ComPtr<IWICBitmapDecoder> decoder;
    auto wide = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
    auto hr = factory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &decoder);
    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND && isHeifFile(path))
        throw Error("HEIC decoding is unavailable. Install HEIF Image Extensions and the HEVC "
                    "Video Extensions from Microsoft Store, then reopen the image.");
    check(hr, "Cannot decode image");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "Cannot decode HEIC primary frame");
    UINT width = 0, height = 0;
    check(frame->GetSize(&width, &height), "Cannot read image dimensions");
    require(width > 0 && height > 0 && width <= MaxSide && height <= MaxSide &&
                qint64(width) * height <= documentPixelBudget() &&
                qint64(width) * height * 4 <= UINT_MAX,
            "WIC image exceeds dimension or memory limits");
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter), "Cannot create WIC pixel converter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                nullptr, 0, WICBitmapPaletteTypeCustom),
          "Cannot convert WIC pixels");
    QImage image(int(width), int(height), QImage::Format_RGBA8888);
    require(!image.isNull(), "Not enough memory for HEIC image");
    check(converter->CopyPixels(nullptr, UINT(image.bytesPerLine()), UINT(image.sizeInBytes()),
                                image.bits()),
          "Cannot read WIC image pixels");
    UINT count = 0;
    if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &count)) && count == 1) {
        ComPtr<IWICColorContext> context;
        if (SUCCEEDED(factory->CreateColorContext(&context))) {
            IWICColorContext *contexts[]{context.Get()};
            UINT actual = 0;
            if (SUCCEEDED(frame->GetColorContexts(1, contexts, &actual))) {
                WICColorContextType type;
                UINT bytes = 0;
                if (SUCCEEDED(context->GetType(&type)) && type == WICColorContextProfile &&
                    SUCCEEDED(context->GetProfileBytes(0, nullptr, &bytes)) &&
                    bytes <= 16 * 1024 * 1024) {
                    QByteArray profile(bytes, Qt::Uninitialized);
                    if (SUCCEEDED(context->GetProfileBytes(
                            bytes, reinterpret_cast<BYTE *>(profile.data()), &bytes))) {
                        auto space = QColorSpace::fromIccProfile(profile);
                        if (space.isValid()) {
                            image.setColorSpace(space);
                            image.convertToColorSpace(QColorSpace::SRgb);
                        }
                    }
                }
            }
        }
    }
    double dpiX = 72, dpiY = 72;
    if (SUCCEEDED(frame->GetResolution(&dpiX, &dpiY)) && std::isfinite(dpiX) &&
        std::isfinite(dpiY) && dpiX > 0 && dpiX <= 9600 && dpiY > 0 && dpiY <= 9600) {
        image.setDotsPerMeterX(int(std::lround(dpiX / .0254)));
        image.setDotsPerMeterY(int(std::lround(dpiY / .0254)));
    }
    ComPtr<IWICMetadataQueryReader> metadata;
    int orientation = 1;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
        for (auto query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
            PROPVARIANT value{};
            if (SUCCEEDED(metadata->GetMetadataByName(query, &value)) && value.vt == VT_UI2)
                orientation = value.uiVal;
            PropVariantClear(&value);
            if (orientation != 1)
                break;
        }
    }
    QTransform transform;
    switch (orientation) {
    case 2:
        image = image.transformed(QTransform::fromScale(-1, 1));
        break;
    case 3:
        transform.rotate(180);
        break;
    case 4:
        image = image.transformed(QTransform::fromScale(1, -1));
        break;
    case 5:
        image = image.transformed(QTransform::fromScale(-1, 1));
        transform.rotate(270);
        break;
    case 6:
        transform.rotate(90);
        break;
    case 7:
        image = image.transformed(QTransform::fromScale(-1, 1));
        transform.rotate(90);
        break;
    case 8:
        transform.rotate(270);
        break;
    }
    if (!transform.isIdentity())
        image = image.transformed(transform);
    return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
#else
    Q_UNUSED(path);
    throw Error("WIC image import is available only on Windows");
#endif
}
} // namespace compositor
