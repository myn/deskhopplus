#include "clipboard_image.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>
#include <objidl.h>
#include <gdiplus.h>

namespace deskhop {
namespace {

struct Pixels {
    UINT width{0};
    UINT height{0};
    int32_t ppm_x{0};
    int32_t ppm_y{0};
    std::vector<uint8_t> bgra;
};

constexpr double kMetresPerInch = 0.0254;

bool checked_image_bytes(UINT width, UINT height, size_t &bytes) {
    if (width == 0 || height == 0 || width > std::numeric_limits<size_t>::max() / 4) return false;
    const size_t stride = static_cast<size_t>(width) * 4;
    if (height > std::numeric_limits<size_t>::max() / stride) return false;
    bytes = stride * height;
    return true;
}

bool png_encoder_clsid(CLSID &out) {
    UINT count = 0;
    UINT bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    if (bytes == 0) return false;
    std::vector<uint8_t> storage(bytes);
    auto *encoders = reinterpret_cast<Gdiplus::ImageCodecInfo *>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            out = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

uint8_t masked_channel(uint32_t pixel, uint32_t mask, uint8_t absent) {
    if (mask == 0) return absent;
    unsigned shift = 0;
    while (((mask >> shift) & 1U) == 0U) ++shift;
    const uint32_t value_mask = mask >> shift;
    const uint32_t value = (pixel & mask) >> shift;
    return static_cast<uint8_t>((value * 255U + value_mask / 2U) / value_mask);
}

bool decode_dib(const std::vector<uint8_t> &dib, Pixels &out) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    BITMAPINFOHEADER header{};
    std::memcpy(&header, dib.data(), sizeof(header));
    if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biSize > dib.size() ||
        header.biWidth <= 0 || header.biHeight == 0 || header.biHeight == LONG_MIN ||
        header.biPlanes != 1 || (header.biBitCount != 24 && header.biBitCount != 32))
        return false;

    uint32_t red_mask = 0x00ff0000;
    uint32_t green_mask = 0x0000ff00;
    uint32_t blue_mask = 0x000000ff;
    uint32_t alpha_mask = 0;
    size_t pixel_offset = header.biSize;
    if (header.biCompression == BI_BITFIELDS) {
        if (header.biBitCount != 32) return false;
        if (header.biSize >= sizeof(BITMAPV5HEADER)) {
            BITMAPV5HEADER v5{};
            std::memcpy(&v5, dib.data(), sizeof(v5));
            red_mask = v5.bV5RedMask;
            green_mask = v5.bV5GreenMask;
            blue_mask = v5.bV5BlueMask;
            alpha_mask = v5.bV5AlphaMask;
        } else {
            constexpr size_t masks_size = 3 * sizeof(DWORD);
            if (pixel_offset > dib.size() - std::min(dib.size(), masks_size)) return false;
            if (dib.size() - pixel_offset < masks_size) return false;
            std::memcpy(&red_mask, dib.data() + pixel_offset, sizeof(red_mask));
            std::memcpy(&green_mask, dib.data() + pixel_offset + sizeof(DWORD), sizeof(green_mask));
            std::memcpy(&blue_mask, dib.data() + pixel_offset + 2 * sizeof(DWORD), sizeof(blue_mask));
            pixel_offset += masks_size;
        }
    } else if (header.biCompression != BI_RGB) {
        return false;
    }

    const UINT width = static_cast<UINT>(header.biWidth);
    const UINT height = static_cast<UINT>(header.biHeight < 0 ? -header.biHeight : header.biHeight);
    const size_t source_stride =
        ((static_cast<size_t>(width) * header.biBitCount + 31U) / 32U) * 4U;
    if (height > std::numeric_limits<size_t>::max() / source_stride) return false;
    const size_t source_bytes = source_stride * height;
    if (pixel_offset > dib.size() || source_bytes > dib.size() - pixel_offset) return false;
    size_t output_bytes = 0;
    if (!checked_image_bytes(width, height, output_bytes)) return false;

    out.width = width;
    out.height = height;
    out.ppm_x = header.biXPelsPerMeter;
    out.ppm_y = header.biYPelsPerMeter;
    out.bgra.resize(output_bytes);
    const bool top_down = header.biHeight < 0;
    for (UINT y = 0; y < height; ++y) {
        const UINT source_y = top_down ? y : height - 1U - y;
        const uint8_t *source = dib.data() + pixel_offset + source_stride * source_y;
        uint8_t *destination = out.bgra.data() + static_cast<size_t>(width) * 4U * y;
        for (UINT x = 0; x < width; ++x) {
            if (header.biBitCount == 24) {
                destination[x * 4] = source[x * 3];
                destination[x * 4 + 1] = source[x * 3 + 1];
                destination[x * 4 + 2] = source[x * 3 + 2];
                destination[x * 4 + 3] = 255;
            } else {
                uint32_t pixel = 0;
                std::memcpy(&pixel, source + x * 4, sizeof(pixel));
                destination[x * 4] = masked_channel(pixel, blue_mask, 0);
                destination[x * 4 + 1] = masked_channel(pixel, green_mask, 0);
                destination[x * 4 + 2] = masked_channel(pixel, red_mask, 0);
                destination[x * 4 + 3] = masked_channel(pixel, alpha_mask, 255);
            }
        }
    }
    return true;
}

std::vector<uint8_t> encode_png(Pixels &pixels) {
    std::vector<uint8_t> result;
    Gdiplus::Bitmap image(static_cast<INT>(pixels.width), static_cast<INT>(pixels.height),
                          static_cast<INT>(pixels.width * 4), PixelFormat32bppARGB,
                          pixels.bgra.data());
    if (image.GetLastStatus() != Gdiplus::Ok) return result;
    if (pixels.ppm_x > 0 && pixels.ppm_y > 0) {
        image.SetResolution(static_cast<Gdiplus::REAL>(pixels.ppm_x * kMetresPerInch),
                            static_cast<Gdiplus::REAL>(pixels.ppm_y * kMetresPerInch));
    }
    CLSID encoder{};
    if (!png_encoder_clsid(encoder)) return result;
    IStream *stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK) return result;
    if (image.Save(stream, &encoder, nullptr) == Gdiplus::Ok) {
        HGLOBAL memory = nullptr;
        if (GetHGlobalFromStream(stream, &memory) == S_OK) {
            STATSTG stat{};
            const SIZE_T size = stream->Stat(&stat, STATFLAG_NONAME) == S_OK
                                    ? static_cast<SIZE_T>(stat.cbSize.QuadPart)
                                    : 0;
            if (const auto *bytes = static_cast<const uint8_t *>(GlobalLock(memory))) {
                result.assign(bytes, bytes + size);
                GlobalUnlock(memory);
            }
        }
    }
    stream->Release();
    return result;
}

bool decode_png(const std::vector<uint8_t> &png, Pixels &out) {
    if (png.empty()) return false;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (memory == nullptr) return false;
    void *destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, png.data(), png.size());
    GlobalUnlock(memory);
    IStream *stream = nullptr;
    if (CreateStreamOnHGlobal(memory, TRUE, &stream) != S_OK) {
        GlobalFree(memory);
        return false;
    }
    Gdiplus::Bitmap image(stream);
    bool ok = image.GetLastStatus() == Gdiplus::Ok && image.GetWidth() != 0 && image.GetHeight() != 0;
    size_t bytes = 0;
    if (ok) ok = checked_image_bytes(image.GetWidth(), image.GetHeight(), bytes);
    if (ok) {
        out.width = image.GetWidth();
        out.height = image.GetHeight();
        const Gdiplus::REAL dpi_x = image.GetHorizontalResolution();
        const Gdiplus::REAL dpi_y = image.GetVerticalResolution();
        out.ppm_x = dpi_x > 0 ? static_cast<int32_t>(std::lround(dpi_x / kMetresPerInch)) : 0;
        out.ppm_y = dpi_y > 0 ? static_cast<int32_t>(std::lround(dpi_y / kMetresPerInch)) : 0;
        out.bgra.resize(bytes);
        Gdiplus::BitmapData locked{};
        const Gdiplus::Rect rectangle(0, 0, static_cast<INT>(out.width),
                                      static_cast<INT>(out.height));
        ok = image.LockBits(&rectangle, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB,
                            &locked) == Gdiplus::Ok;
        if (ok) {
            for (UINT y = 0; y < out.height; ++y) {
                const auto *source = static_cast<const uint8_t *>(locked.Scan0) +
                                     static_cast<ptrdiff_t>(y) * locked.Stride;
                std::memcpy(out.bgra.data() + static_cast<size_t>(y) * out.width * 4U, source,
                            static_cast<size_t>(out.width) * 4U);
            }
            image.UnlockBits(&locked);
        }
    }
    stream->Release();
    return ok;
}

template <typename Header>
std::vector<uint8_t> encode_dib(const Pixels &pixels, Header header) {
    size_t pixel_bytes = 0;
    if (!checked_image_bytes(pixels.width, pixels.height, pixel_bytes)) return {};
    std::vector<uint8_t> result(sizeof(Header) + pixel_bytes);
    std::memcpy(result.data(), &header, sizeof(header));
    std::memcpy(result.data() + sizeof(header), pixels.bgra.data(), pixel_bytes);
    return result;
}

} // namespace

std::vector<uint8_t> dib_to_png(const std::vector<uint8_t> &dib) {
    Pixels pixels;
    return decode_dib(dib, pixels) ? encode_png(pixels) : std::vector<uint8_t>{};
}

std::vector<uint8_t> bitmap_to_png(HBITMAP bitmap) {
    if (bitmap == nullptr) return {};
    Gdiplus::Bitmap image(bitmap, nullptr);
    if (image.GetLastStatus() != Gdiplus::Ok) return {};
    Gdiplus::BitmapData locked{};
    const Gdiplus::Rect rectangle(0, 0, static_cast<INT>(image.GetWidth()),
                                  static_cast<INT>(image.GetHeight()));
    if (image.LockBits(&rectangle, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &locked) !=
        Gdiplus::Ok)
        return {};
    Pixels pixels;
    pixels.width = image.GetWidth();
    pixels.height = image.GetHeight();
    size_t bytes = 0;
    if (!checked_image_bytes(pixels.width, pixels.height, bytes)) {
        image.UnlockBits(&locked);
        return {};
    }
    pixels.bgra.resize(bytes);
    for (UINT y = 0; y < pixels.height; ++y) {
        const auto *source = static_cast<const uint8_t *>(locked.Scan0) +
                             static_cast<ptrdiff_t>(y) * locked.Stride;
        std::memcpy(pixels.bgra.data() + static_cast<size_t>(y) * pixels.width * 4U, source,
                    static_cast<size_t>(pixels.width) * 4U);
    }
    image.UnlockBits(&locked);
    const Gdiplus::REAL dpi_x = image.GetHorizontalResolution();
    const Gdiplus::REAL dpi_y = image.GetVerticalResolution();
    pixels.ppm_x = dpi_x > 0 ? static_cast<int32_t>(std::lround(dpi_x / kMetresPerInch)) : 0;
    pixels.ppm_y = dpi_y > 0 ? static_cast<int32_t>(std::lround(dpi_y / kMetresPerInch)) : 0;
    return encode_png(pixels);
}

std::vector<uint8_t> capture_clipboard_image(ClipboardImageFormat format,
                                             const std::vector<uint8_t> &bytes) {
    if (format == ClipboardImageFormat::Png) return bytes;
    if (format == ClipboardImageFormat::DibV5 || format == ClipboardImageFormat::Dib)
        return dib_to_png(bytes);
    return {};
}

std::vector<uint8_t> png_to_dibv5(const std::vector<uint8_t> &png) {
    Pixels pixels;
    if (!decode_png(png, pixels)) return {};
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(pixels.width);
    header.bV5Height = -static_cast<LONG>(pixels.height);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5XPelsPerMeter = pixels.ppm_x;
    header.bV5YPelsPerMeter = pixels.ppm_y;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    header.bV5CSType = 0x73524742; /* LCS_sRGB without a multi-character literal. */
    header.bV5Intent = LCS_GM_IMAGES;
    header.bV5SizeImage = static_cast<DWORD>(pixels.bgra.size());
    return encode_dib(pixels, header);
}

std::vector<uint8_t> png_to_dib(const std::vector<uint8_t> &png) {
    Pixels pixels;
    if (!decode_png(png, pixels)) return {};
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = static_cast<LONG>(pixels.width);
    header.biHeight = -static_cast<LONG>(pixels.height);
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(pixels.bgra.size());
    header.biXPelsPerMeter = pixels.ppm_x;
    header.biYPelsPerMeter = pixels.ppm_y;
    return encode_dib(pixels, header);
}

ClipboardImageRepresentations clipboard_image_representations(const std::vector<uint8_t> &png) {
    return {png, png_to_dibv5(png), png_to_dib(png)};
}

} // namespace deskhop
