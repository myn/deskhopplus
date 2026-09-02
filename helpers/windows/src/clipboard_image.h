#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace deskhop {

enum class ClipboardImageFormat { None, Png, DibV5, Dib, Bitmap };

/* The copy-side platform boundary. Registered PNG is the only representation
   that is already the channel's image encoding, so synthesized formats never
   displace it. */
constexpr ClipboardImageFormat select_clipboard_image_format(bool png, bool dibv5, bool dib,
                                                              bool bitmap) {
    if (png) return ClipboardImageFormat::Png;
    if (dibv5) return ClipboardImageFormat::DibV5;
    if (dib) return ClipboardImageFormat::Dib;
    if (bitmap) return ClipboardImageFormat::Bitmap;
    return ClipboardImageFormat::None;
}

/* Clipboard DIB blocks contain the BITMAPINFO header followed by pixels; no
   BITMAPFILEHEADER is present. Invalid or unsupported input returns empty. */
std::vector<uint8_t> dib_to_png(const std::vector<uint8_t> &dib);
std::vector<uint8_t> bitmap_to_png(HBITMAP bitmap);

/* Apply the already-selected capture format. PNG is returned byte-identically;
   DIB formats cross the platform boundary once by encoding to PNG. */
std::vector<uint8_t> capture_clipboard_image(ClipboardImageFormat format,
                                             const std::vector<uint8_t> &bytes);

/* Compatibility representations built from received PNG bytes. DIBV5 is the
   fidelity representation (explicit RGBA masks and pixels-per-meter); DIB is
   retained for applications that do not consume DIBV5. Both are top-down. */
std::vector<uint8_t> png_to_dibv5(const std::vector<uint8_t> &png);
std::vector<uint8_t> png_to_dib(const std::vector<uint8_t> &png);

struct ClipboardImageRepresentations {
    std::vector<uint8_t> png;
    std::vector<uint8_t> dibv5;
    std::vector<uint8_t> dib;
};

/* DIBV5 carries RGBA and DIB covers older consumers. CF_BITMAP is omitted:
   its GDI object has no useful resolution metadata and GetHBITMAP flattens
   transparency, while no compatibility test has established a remaining need. */
constexpr bool kPublishCfBitmap = false;

/* Build one publication transaction. The PNG member is copied byte-for-byte;
   only the compatibility members are decoded and synthesized. */
ClipboardImageRepresentations clipboard_image_representations(const std::vector<uint8_t> &png);

} // namespace deskhop
