#include "clipboard.h"

#include <cstring>
#include <cwchar>
#include <propidl.h>
#include <gdiplus.h>
#include <objidl.h>

namespace deskhop {

namespace {

/*
 * OpenClipboard has no built-in wait, so this is the wait. Five attempts at a
 * widening delay is 150 ms in the worst case — long enough for the momentary
 * holds an editor or a browser takes, and short enough that the message loop
 * this runs on is still well inside the session's three-second deadline.
 */
constexpr int kOpenAttempts = 5;
constexpr DWORD kFirstRetryMs = 10;

bool png_encoder_clsid(CLSID &out) {
    UINT count = 0, bytes = 0;
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

std::vector<uint8_t> bitmap_to_png(HBITMAP bitmap) {
    std::vector<uint8_t> result;
    Gdiplus::Bitmap image(bitmap, nullptr);
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

HBITMAP png_to_bitmap(const std::vector<uint8_t> &png) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!memory) return nullptr;
    void *destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); return nullptr; }
    std::memcpy(destination, png.data(), png.size());
    GlobalUnlock(memory);
    IStream *stream = nullptr;
    if (CreateStreamOnHGlobal(memory, TRUE, &stream) != S_OK) {
        GlobalFree(memory);
        return nullptr;
    }
    Gdiplus::Bitmap image(stream);
    HBITMAP bitmap = nullptr;
    if (image.GetLastStatus() == Gdiplus::Ok)
        image.GetHBITMAP(Gdiplus::Color(255, 255, 255), &bitmap);
    stream->Release();
    return bitmap;
}

std::wstring utf8_to_wide(const uint8_t *bytes, size_t len) {
    if (len == 0) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char *>(bytes),
                                          static_cast<int>(len), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring out(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char *>(bytes), static_cast<int>(len),
                        out.data(), chars);
    return out;
}

std::vector<uint8_t> wide_to_utf8(const wchar_t *text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {}; /* just the terminator, or nothing */
    std::vector<uint8_t> out(static_cast<size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, text, -1, reinterpret_cast<char *>(out.data()), bytes, nullptr,
                        nullptr);
    /* The conversion includes the terminating NUL. The clipboard's text is
       NUL-terminated by convention and the payload is not: carrying it would
       put a byte on the wire that was never copied, and fidelity means the far
       end pastes exactly what was selected. */
    out.pop_back();
    return out;
}

} // namespace

void Clipboard::attach(HWND window, Callbacks callbacks) {
    window_ = window;
    callbacks_ = std::move(callbacks);
    Gdiplus::GdiplusStartupInput gdiplus_input;
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        gdiplus_token_ = 0;
        if (callbacks_.log) callbacks_.log("the image codec could not start; images will not be sent");
    }
    /* Whatever is on the clipboard when the helper starts was not copied *now*
       and is not this helper's to send. Both counters start there. */
    self_sequence_ = GetClipboardSequenceNumber();
    handled_sequence_ = self_sequence_;
    listening_ = AddClipboardFormatListener(window) != FALSE;
    if (!listening_ && callbacks_.log)
        callbacks_.log("the clipboard listener could not be registered; copies made on this "
                       "computer will not be sent");
}

void Clipboard::detach() {
    if (listening_ && window_ != nullptr) RemoveClipboardFormatListener(window_);
    listening_ = false;
    window_ = nullptr;
    if (gdiplus_token_ != 0) Gdiplus::GdiplusShutdown(gdiplus_token_);
    gdiplus_token_ = 0;
}

bool Clipboard::handle(UINT message) {
    if (message == WM_RENDERFORMAT && lazy_image_id_ != 0) {
        const uint32_t id = lazy_image_id_;
        lazy_image_id_ = 0;
        if (!callbacks_.request_image) return true;
        const auto png = callbacks_.request_image(id, lazy_image_total_);
        if (!png) {
            if (callbacks_.log) callbacks_.log("the lazy image did not arrive before the paste timed out");
            return true;
        }
        HBITMAP bitmap = png_to_bitmap(*png);
        if (!bitmap || SetClipboardData(CF_BITMAP, bitmap) == nullptr) {
            if (bitmap) DeleteObject(bitmap);
            if (callbacks_.log) callbacks_.log("the lazy image arrived but could not be rendered");
        }
        return true;
    }
    if (message != WM_CLIPBOARDUPDATE) return false;

    /*
     * The message is *posted*, so what it names is "something changed", not
     * which change. By the time it is handled the sequence number is whatever
     * the clipboard holds now — so two messages queued behind one change both
     * read the same clipboard, and without this the same payload crosses the
     * link twice.
     */
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == handled_sequence_) return true;
    handled_sequence_ = sequence;

    /* Our own write, echoing back. Without this the two helpers hand the same
       payload back and forth for ever. */
    if (sequence != self_sequence_) read_clipboard();
    return true;
}

void Clipboard::lazy_image(uint32_t id, uint64_t total) {
    if (!open_with_retry()) {
        if (callbacks_.log) callbacks_.log("a lazy image offer could not claim the clipboard");
        return;
    }
    EmptyClipboard();
    SetLastError(ERROR_SUCCESS);
    SetClipboardData(CF_BITMAP, nullptr);
    if (GetLastError() != ERROR_SUCCESS) {
        CloseClipboard();
        if (callbacks_.log) callbacks_.log("the clipboard refused a lazy image placeholder");
        return;
    }
    lazy_image_id_ = id;
    lazy_image_total_ = total;
    self_sequence_ = GetClipboardSequenceNumber();
    CloseClipboard();
    if (callbacks_.log)
        callbacks_.log("a lazy image of " + std::to_string(total) + " bytes is ready to paste");
}

void Clipboard::cancel_lazy_image(uint32_t id) {
    if (lazy_image_id_ != id) return;
    lazy_image_id_ = 0;
    if (open_with_retry()) {
        EmptyClipboard();
        self_sequence_ = GetClipboardSequenceNumber();
        CloseClipboard();
    }
    if (callbacks_.log) callbacks_.log("lazy image " + std::to_string(id) +
                                       " was removed before it could be pasted");
}

bool Clipboard::open_with_retry() {
    if (window_ == nullptr) return false;
    DWORD delay = kFirstRetryMs;
    for (int attempt = 1; attempt <= kOpenAttempts; attempt++) {
        if (OpenClipboard(window_)) {
            if (attempt > 1 && callbacks_.log)
                callbacks_.log("the clipboard took " + std::to_string(attempt) +
                               " attempts to open");
            return true;
        }
        if (attempt < kOpenAttempts) {
            Sleep(delay);
            delay *= 2;
        }
    }
    if (callbacks_.log)
        callbacks_.log("another program held the clipboard open through " +
                       std::to_string(kOpenAttempts) + " attempts");
    return false;
}

void Clipboard::read_clipboard() {
    if (!callbacks_.local_copy && !callbacks_.local_image) return;
    /* Ask first so unrelated clipboard formats cost nothing and do not open
       the clipboard against another program that wants it. */
    const bool has_text = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    const bool has_image = gdiplus_token_ != 0 && IsClipboardFormatAvailable(CF_BITMAP) != FALSE;
    if (!has_text && !has_image) return;
    if (!open_with_retry()) return;

    std::vector<uint8_t> payload;
    if (has_image) {
        if (HBITMAP bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)))
            payload = bitmap_to_png(bitmap);
    } else if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto *text = static_cast<const wchar_t *>(GlobalLock(handle))) {
            payload = wide_to_utf8(text);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();

    /*
     * An empty read is not the same as an empty clipboard. The managed laptop
     * runs Trellix DLP, whose block behaviour at the Win32 level is
     * undocumented, so a read that "succeeded" and returned nothing cannot be
     * ruled out (#60) — the spec's standing instruction is to treat a
     * clipboard read failure as an expected state and say so rather than trust
     * it. Nothing is sent, and the reason is in the log.
     */
    if (payload.empty()) {
        if (callbacks_.log)
            callbacks_.log("the clipboard offered content and then read as empty; nothing was sent");
        return;
    }
    if (has_image && callbacks_.local_image) callbacks_.local_image(std::move(payload));
    else if (callbacks_.local_copy) callbacks_.local_copy(std::move(payload));
}

void Clipboard::deliver_image(const std::vector<uint8_t> &png) {
    if (gdiplus_token_ == 0) {
        if (callbacks_.log) callbacks_.log("an arriving image could not be decoded; the image codec is unavailable");
        return;
    }
    HBITMAP bitmap = png_to_bitmap(png);
    if (!bitmap) {
        if (callbacks_.log) callbacks_.log("an arriving image could not be decoded; nothing was written");
        return;
    }
    if (!open_with_retry()) {
        DeleteObject(bitmap);
        if (callbacks_.log) callbacks_.log("the image arrived but the clipboard would not open; it was not written");
        return;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_BITMAP, bitmap) == nullptr) {
        CloseClipboard();
        DeleteObject(bitmap);
        if (callbacks_.log) callbacks_.log("the clipboard refused an arriving image; it is now empty");
        return;
    }
    self_sequence_ = GetClipboardSequenceNumber();
    CloseClipboard();
}

void Clipboard::deliver_text(const std::vector<uint8_t> &utf8) {
    const std::wstring wide = utf8_to_wide(utf8.data(), utf8.size());
    if (wide.empty() && !utf8.empty()) {
        if (callbacks_.log)
            callbacks_.log("a payload of " + std::to_string(utf8.size()) +
                           " bytes would not convert to wide text; nothing was written");
        return;
    }

    /* Allocated before the clipboard is opened, so nothing else is kept
       waiting on this process's allocator. */
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (block == nullptr) {
        if (callbacks_.log) callbacks_.log("no memory for an arriving clipboard payload");
        return;
    }
    if (auto *destination = static_cast<wchar_t *>(GlobalLock(block))) {
        std::memcpy(destination, wide.c_str(), bytes);
        GlobalUnlock(block);
    } else {
        GlobalFree(block);
        if (callbacks_.log) callbacks_.log("an arriving clipboard payload could not be locked");
        return;
    }

    if (!open_with_retry()) {
        GlobalFree(block);
        if (callbacks_.log)
            callbacks_.log("the content arrived but the clipboard would not open; it was not "
                           "written");
        return;
    }

    EmptyClipboard();
    /* A real handle, never nullptr: this slice claims no format it cannot
       immediately produce, which is what keeps a pasting application from ever
       waiting on this helper. See clipboard.h. */
    if (SetClipboardData(CF_UNICODETEXT, block) == nullptr) {
        /* Ownership only transfers on success. On failure it is still ours to
           free, and leaking it would leak every failed payload. */
        CloseClipboard();
        GlobalFree(block);
        /* EmptyClipboard has already run, so what the user had copied is gone
           and the arriving payload was never written. Said plainly rather than
           left to be discovered by pasting nothing. Unlike the macOS side there
           is nothing to put back: emptying frees the handles it held. */
        if (callbacks_.log)
            callbacks_.log("the clipboard refused an arriving payload; it is now empty");
        return;
    }
    /*
     * Read *before* the clipboard is closed, deliberately. Nothing else can
     * change it while this process holds it open, so the number read here is
     * certainly the one our own write produced. Reading it after the close
     * leaves a window in which a foreign copy lands first and gets recorded as
     * ours — and that copy would then never cross the link.
     */
    self_sequence_ = GetClipboardSequenceNumber();
    CloseClipboard();
}

} // namespace deskhop
