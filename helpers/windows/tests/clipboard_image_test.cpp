#include "clipboard_image.h"

#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using deskhop::ClipboardImageFormat;
using deskhop::capture_clipboard_image;
using deskhop::clipboard_image_representations;
using deskhop::dib_to_png;
using deskhop::png_to_dib;
using deskhop::png_to_dibv5;
using deskhop::select_clipboard_image_format;
using deskhop::kPublishCfBitmap;

namespace {

int failures = 0;

#define CHECK(condition, message)                                                                  \
    do {                                                                                            \
        if (!(condition)) {                                                                         \
            std::fprintf(stderr, "FAIL: %s\n", message);                                           \
            ++failures;                                                                             \
        }                                                                                           \
    } while (false)

std::vector<uint8_t> dibv5_fixture(int32_t width, int32_t height, int32_t ppm_x, int32_t ppm_y,
                                   const std::vector<uint8_t> &top_down_bgra) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = width;
    header.bV5Height = -height;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = static_cast<DWORD>(width * height * 4);
    header.bV5XPelsPerMeter = ppm_x;
    header.bV5YPelsPerMeter = ppm_y;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    header.bV5CSType = 0x73524742; /* LCS_sRGB without a multi-character literal. */
    header.bV5Intent = LCS_GM_IMAGES;
    std::vector<uint8_t> bytes(sizeof(header) + top_down_bgra.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), top_down_bgra.data(), top_down_bgra.size());
    return bytes;
}

const BITMAPV5HEADER &v5_header(const std::vector<uint8_t> &dib) {
    return *reinterpret_cast<const BITMAPV5HEADER *>(dib.data());
}

void test_capture_precedence_keeps_original_png() {
    CHECK(select_clipboard_image_format(true, true, true, true) == ClipboardImageFormat::Png,
          "registered PNG did not win over synthesized bitmap formats");
    CHECK(select_clipboard_image_format(false, true, true, true) == ClipboardImageFormat::DibV5,
          "DIBV5 was not the first capture fallback");
    CHECK(select_clipboard_image_format(false, false, true, true) == ClipboardImageFormat::Dib,
          "DIB was not the second capture fallback");
    CHECK(select_clipboard_image_format(false, false, false, true) == ClipboardImageFormat::Bitmap,
          "bitmap was not the final capture fallback");
    const std::vector<uint8_t> original_png = {137, 80, 78, 71, 13, 10, 26, 10, 99, 88};
    CHECK(capture_clipboard_image(ClipboardImageFormat::Png, original_png) == original_png,
          "the capture boundary decoded or replaced an application's original PNG bytes");
}

void test_opaque_high_resolution_dibv5_keeps_dimensions_and_resolution() {
    const std::vector<uint8_t> pixels = {
        30, 20, 10, 255, 70, 60, 50, 255, 110, 100, 90, 255,
        3,  2,  1,  255, 6,  5,  4,  255, 9,   8,   7,  255,
    };
    const auto round_trip = png_to_dibv5(dib_to_png(dibv5_fixture(3, 2, 11811, 5906, pixels)));
    CHECK(round_trip.size() == sizeof(BITMAPV5HEADER) + pixels.size(),
          "an opaque high-resolution DIBV5 did not encode as PNG");
    if (round_trip.size() < sizeof(BITMAPV5HEADER) + pixels.size()) return;
    const auto &header = v5_header(round_trip);
    CHECK(header.bV5Width == 3 && header.bV5Height == -2,
          "opaque DIBV5 conversion resampled or changed row orientation");
    CHECK(header.bV5XPelsPerMeter >= 11810 && header.bV5XPelsPerMeter <= 11812 &&
              header.bV5YPelsPerMeter >= 5905 && header.bV5YPelsPerMeter <= 5907,
          "PNG resolution metadata did not return in DIBV5 pixels-per-meter fields");
}

void test_dibv5_round_trip_keeps_nontrivial_alpha() {
    const std::vector<uint8_t> pixels = {
        30, 20, 10, 40, 70, 60, 50, 128, 110, 100, 90, 200,
        3,  2,  1,  255, 6,  5,  4,  17,  9,   8,   7,  0,
    };
    const auto source = dibv5_fixture(3, 2, 11811, 5906, pixels);
    const auto png = dib_to_png(source);
    CHECK(!png.empty(), "a DIBV5 with alpha did not encode as PNG");

    const auto round_trip = png_to_dibv5(png);
    CHECK(round_trip.size() == sizeof(BITMAPV5HEADER) + pixels.size(),
          "PNG did not publish a complete DIBV5 representation");
    if (round_trip.size() < sizeof(BITMAPV5HEADER) + pixels.size()) return;
    const auto &header = v5_header(round_trip);
    CHECK(header.bV5Width == 3 && header.bV5Height == -2,
          "DIBV5 conversion resampled or changed row orientation");
    CHECK(header.bV5AlphaMask == 0xff000000, "published DIBV5 has no explicit alpha mask");
    CHECK(std::memcmp(round_trip.data() + sizeof(BITMAPV5HEADER), pixels.data(), pixels.size()) == 0,
          "DIBV5 RGBA pixels or alpha changed during PNG conversion");
}

void test_bottom_up_dib_keeps_dimensions_and_visual_row_order() {
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = 2;
    header.biHeight = 2;
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    header.biSizeImage = 16;
    const uint8_t bottom_up[] = {
        90, 80, 70, 120, 110, 100, 0, 0,
        30, 20, 10, 60,  50,  40,  0, 0,
    };
    std::vector<uint8_t> source(sizeof(header) + sizeof(bottom_up));
    std::memcpy(source.data(), &header, sizeof(header));
    std::memcpy(source.data() + sizeof(header), bottom_up, sizeof(bottom_up));

    const auto round_trip = png_to_dibv5(dib_to_png(source));
    CHECK(round_trip.size() == sizeof(BITMAPV5HEADER) + 16,
          "a DIB did not retain its dimensions through PNG");
    if (round_trip.size() < sizeof(BITMAPV5HEADER) + 16) return;
    const uint8_t expected_top_down[] = {
        30, 20, 10, 255, 60, 50, 40, 255,
        90, 80, 70, 255, 120, 110, 100, 255,
    };
    CHECK(std::memcmp(round_trip.data() + sizeof(BITMAPV5HEADER), expected_top_down,
                      sizeof(expected_top_down)) == 0,
          "a bottom-up DIB's visual row order changed");
}

void test_publication_builds_dib_and_dibv5_without_changing_png() {
    const auto source = dibv5_fixture(1, 1, 3780, 3780, {33, 22, 11, 77});
    const auto png = dib_to_png(source);
    const auto representations = clipboard_image_representations(png);
    CHECK(representations.png == png, "publication changed the received registered PNG bytes");
    CHECK(!representations.dibv5.empty(), "publication did not build CF_DIBV5");
    CHECK(!representations.dib.empty(), "publication did not build CF_DIB for compatibility");
    CHECK(!kPublishCfBitmap,
          "CF_BITMAP should remain omitted until compatibility testing justifies alpha/DPI loss");
    CHECK(v5_header(representations.dibv5).bV5Width == 1 &&
              v5_header(representations.dibv5).bV5Height == -1,
          "publication changed PNG pixel dimensions");
}

} // namespace

int main() {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        std::fprintf(stderr, "GDI+ could not start\n");
        return 1;
    }
    test_capture_precedence_keeps_original_png();
    test_opaque_high_resolution_dibv5_keeps_dimensions_and_resolution();
    test_dibv5_round_trip_keeps_nontrivial_alpha();
    test_bottom_up_dib_keeps_dimensions_and_visual_row_order();
    test_publication_builds_dib_and_dibv5_without_changing_png();
    Gdiplus::GdiplusShutdown(token);
    if (failures != 0) return 1;
    std::printf("clipboard image tests passed\n");
    return 0;
}
