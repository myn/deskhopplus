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

std::string clipboard_state(HWND helper) {
    return "sequence=" + std::to_string(GetClipboardSequenceNumber()) +
           " owner=" + std::to_string(reinterpret_cast<uintptr_t>(GetClipboardOwner())) +
           " helper=" + std::to_string(reinterpret_cast<uintptr_t>(helper)) +
           " opener=" + std::to_string(reinterpret_cast<uintptr_t>(GetOpenClipboardWindow()));
}

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

std::vector<uint8_t> global_bytes(HANDLE handle) {
    if (handle == nullptr) return {};
    const SIZE_T size = GlobalSize(handle);
    if (size == 0) return {};
    const auto *bytes = static_cast<const uint8_t *>(GlobalLock(handle));
    if (bytes == nullptr) return {};
    std::vector<uint8_t> result(bytes, bytes + size);
    GlobalUnlock(handle);
    return result;
}

HGLOBAL bytes_to_global(const std::vector<uint8_t> &bytes) {
    if (bytes.empty()) return nullptr;
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (block == nullptr) return nullptr;
    void *destination = GlobalLock(block);
    if (destination == nullptr) {
        GlobalFree(block);
        return nullptr;
    }
    std::memcpy(destination, bytes.data(), bytes.size());
    GlobalUnlock(block);
    return block;
}

} // namespace

void Clipboard::attach(HWND window, Callbacks callbacks) {
    window_ = window;
    callbacks_ = std::move(callbacks);
    /* Windows applications including the Snipping Tool commonly publish the
       registered PNG format alongside CF_BITMAP. Prefer those original bytes:
       the bitmap fallback can discard alpha and other image fidelity. */
    png_format_ = RegisterClipboardFormatW(L"PNG");
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
    png_format_ = 0;
}

bool Clipboard::load_lazy_image() {
    if (!lazy_image_png_.empty()) return true;
    const uint32_t id = lazy_image_id_;
    if (id == 0 || !callbacks_.request_image) return false;
    if (callbacks_.log)
        callbacks_.log("[clipboard-debug] requesting lazy image id=" + std::to_string(id) +
                       " bytes=" + std::to_string(lazy_image_total_) + " " +
                       clipboard_state(window_));
    const auto png = callbacks_.request_image(id, lazy_image_total_);
    if (!png) {
        if (callbacks_.log)
            callbacks_.log("the lazy image did not arrive before the paste timed out");
        return false;
    }
    lazy_image_png_ = *png;
    lazy_image_id_ = 0;
    return true;
}

bool Clipboard::handle(UINT message, WPARAM parameter) {
    if (message == WM_DESTROYCLIPBOARD) {
        /* Another owner replaced our lazy placeholder. Forget it before a
           later transfer cancellation can empty that owner's newer copy. */
        const uint32_t replaced = lazy_image_id_;
        if (callbacks_.log)
            callbacks_.log("[clipboard-debug] WM_DESTROYCLIPBOARD lazy_id=" +
                           std::to_string(replaced) + " cached_bytes=" +
                           std::to_string(lazy_image_png_.size()) + " " +
                           clipboard_state(window_));
        lazy_image_id_ = 0;
        lazy_image_total_ = 0;
        lazy_image_png_.clear();
        if (replaced != 0 && callbacks_.lazy_image_replaced)
            callbacks_.lazy_image_replaced(replaced);
        return true;
    }
    if (message == WM_RENDERFORMAT && callbacks_.log)
        callbacks_.log("[clipboard-debug] WM_RENDERFORMAT format=" +
                       std::to_string(parameter) + " lazy_id=" +
                       std::to_string(lazy_image_id_) + " cached_bytes=" +
                       std::to_string(lazy_image_png_.size()) + " " +
                       clipboard_state(window_));
    if (message == WM_RENDERFORMAT && (lazy_image_id_ != 0 || !lazy_image_png_.empty())) {
        if (!load_lazy_image()) return true;
        if (parameter == png_format_) {
            HGLOBAL block = bytes_to_global(lazy_image_png_);
            if (block == nullptr || SetClipboardData(png_format_, block) == nullptr) {
                if (block) GlobalFree(block);
                if (callbacks_.log)
                    callbacks_.log("the lazy image arrived but its PNG format could not be rendered");
            }
        } else if (parameter == CF_BITMAP) {
            HBITMAP bitmap = png_to_bitmap(lazy_image_png_);
            if (!bitmap || SetClipboardData(CF_BITMAP, bitmap) == nullptr) {
                if (bitmap) DeleteObject(bitmap);
                if (callbacks_.log)
                    callbacks_.log("the lazy image arrived but its bitmap format could not be rendered");
            }
        }
        /* Filling a promised format is our clipboard write too. If Windows
           advances the sequence for it, its update must not echo the image
           back across the channel as a new local copy. */
        self_sequence_ = GetClipboardSequenceNumber();
        return true;
    }
    if (message == WM_RENDERALLFORMATS && callbacks_.log)
        callbacks_.log("[clipboard-debug] WM_RENDERALLFORMATS lazy_id=" +
                       std::to_string(lazy_image_id_) + " cached_bytes=" +
                       std::to_string(lazy_image_png_.size()) + " " +
                       clipboard_state(window_));
    if (message == WM_RENDERALLFORMATS &&
        (lazy_image_id_ != 0 || !lazy_image_png_.empty())) {
        /* Windows is about to destroy the owner window. Materialise every
           promised representation so the clipboard survives helper exit. */
        if (!load_lazy_image()) return true;
        const std::vector<uint8_t> png = lazy_image_png_;
        if (!open_with_retry()) return true;
        if (GetClipboardOwner() != window_) {
            CloseClipboard();
            return true;
        }
        EmptyClipboard();
        HGLOBAL block = png_format_ != 0 ? bytes_to_global(png) : nullptr;
        if (block != nullptr && SetClipboardData(png_format_, block) == nullptr)
            GlobalFree(block);
        HBITMAP bitmap = gdiplus_token_ != 0 ? png_to_bitmap(png) : nullptr;
        if (bitmap != nullptr && SetClipboardData(CF_BITMAP, bitmap) == nullptr)
            DeleteObject(bitmap);
        self_sequence_ = GetClipboardSequenceNumber();
        CloseClipboard();
        lazy_image_id_ = 0;
        lazy_image_total_ = 0;
        lazy_image_png_.clear();
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
    if (trace_lazy_lifecycle_ && callbacks_.log)
        callbacks_.log("[clipboard-debug] WM_CLIPBOARDUPDATE sequence=" +
                       std::to_string(sequence) + " handled=" +
                       std::to_string(handled_sequence_) + " self=" +
                       std::to_string(self_sequence_) + " owner=" +
                       std::to_string(reinterpret_cast<uintptr_t>(GetClipboardOwner())) +
                       " helper=" + std::to_string(reinterpret_cast<uintptr_t>(window_)) +
                       " has_text=" +
                       std::to_string(IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE) +
                       " has_png=" +
                       std::to_string(png_format_ != 0 &&
                                      IsClipboardFormatAvailable(png_format_) != FALSE) +
                       " has_bitmap=" +
                       std::to_string(IsClipboardFormatAvailable(CF_BITMAP) != FALSE));
    if (sequence == handled_sequence_) return true;
    handled_sequence_ = sequence;

    /* Our own write, echoing back. Without this the two helpers hand the same
       payload back and forth for ever. */
    if (sequence != self_sequence_) {
        read_clipboard();
        /* The first external update is the replacement under investigation.
           One line records it; ordinary copies after it stay off the helper's
           synchronously flushed diagnostic path. */
        trace_lazy_lifecycle_ = false;
    }
    return true;
}

void Clipboard::lazy_image(uint32_t id, uint64_t total) {
    trace_lazy_lifecycle_ = true;
    if (callbacks_.log)
        callbacks_.log("[clipboard-debug] claiming lazy image id=" + std::to_string(id) +
                       " bytes=" + std::to_string(total) + " before_" +
                       clipboard_state(window_));
    if (!open_with_retry()) {
        if (callbacks_.log) callbacks_.log("a lazy image offer could not claim the clipboard");
        return;
    }
    EmptyClipboard();
    bool advertised = false;
    if (png_format_ != 0) {
        SetLastError(ERROR_SUCCESS);
        SetClipboardData(png_format_, nullptr);
        advertised = GetLastError() == ERROR_SUCCESS;
    }
    SetLastError(ERROR_SUCCESS);
    SetClipboardData(CF_BITMAP, nullptr);
    advertised = advertised || GetLastError() == ERROR_SUCCESS;
    if (!advertised) {
        CloseClipboard();
        if (callbacks_.log) callbacks_.log("the clipboard refused a lazy image placeholder");
        return;
    }
    lazy_image_id_ = id;
    lazy_image_total_ = total;
    lazy_image_png_.clear();
    self_sequence_ = GetClipboardSequenceNumber();
    CloseClipboard();
    if (callbacks_.log)
        callbacks_.log("a lazy image id=" + std::to_string(id) + " of " +
                       std::to_string(total) + " bytes is ready to paste; " +
                       clipboard_state(window_));
}

void Clipboard::cancel_lazy_image(uint32_t id) {
    if (callbacks_.log)
        callbacks_.log("[clipboard-debug] cancel lazy image id=" + std::to_string(id) +
                       " active_id=" + std::to_string(lazy_image_id_) + " " +
                       clipboard_state(window_));
    if (lazy_image_id_ != id) return;
    lazy_image_id_ = 0;
    lazy_image_total_ = 0;
    lazy_image_png_.clear();
    if (GetClipboardOwner() == window_ && open_with_retry()) {
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
    const bool has_png = png_format_ != 0 && IsClipboardFormatAvailable(png_format_) != FALSE;
    const bool has_bitmap = gdiplus_token_ != 0 && IsClipboardFormatAvailable(CF_BITMAP) != FALSE;
    const bool has_image = has_png || has_bitmap;
    if (!has_text && !has_image) return;
    if (!open_with_retry()) return;

    std::vector<uint8_t> payload;
    if (has_png) {
        payload = global_bytes(GetClipboardData(png_format_));
    }
    if (payload.empty() && has_bitmap) {
        if (HBITMAP bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)))
            payload = bitmap_to_png(bitmap);
    }
    if (!has_image) {
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
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
    HGLOBAL png_block = png_format_ != 0 ? bytes_to_global(png) : nullptr;
    HBITMAP bitmap = gdiplus_token_ != 0 ? png_to_bitmap(png) : nullptr;
    if (png_block == nullptr && bitmap == nullptr) {
        if (callbacks_.log)
            callbacks_.log("an arriving image had no clipboard representation; nothing was written");
        return;
    }
    if (!open_with_retry()) {
        if (bitmap) DeleteObject(bitmap);
        if (png_block) GlobalFree(png_block);
        if (callbacks_.log) callbacks_.log("the image arrived but the clipboard would not open; it was not written");
        return;
    }
    EmptyClipboard();
    bool png_written = false;
    if (png_block != nullptr) {
        png_written = SetClipboardData(png_format_, png_block) != nullptr;
        if (!png_written) GlobalFree(png_block);
    }
    const bool bitmap_written = bitmap != nullptr && SetClipboardData(CF_BITMAP, bitmap) != nullptr;
    if (bitmap != nullptr && !bitmap_written) DeleteObject(bitmap);
    if (!png_written && !bitmap_written) {
        CloseClipboard();
        if (callbacks_.log) callbacks_.log("the clipboard refused an arriving image; it is now empty");
        return;
    }
    if (!bitmap_written && callbacks_.log)
        callbacks_.log("the arriving PNG was written, but its bitmap compatibility format was refused");
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
