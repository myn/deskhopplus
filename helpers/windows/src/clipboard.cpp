#include "clipboard.h"

#include <cstring>
#include <cwchar>
#include <propidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <objidl.h>

#include "clipboard_update.h"
#include "clipboard_image.h"

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

/* The same conversion, as a std::string — what a file name is carried in. */
std::string wide_to_utf8_string(const wchar_t *text) {
    const std::vector<uint8_t> bytes = wide_to_utf8(text);
    return std::string(bytes.begin(), bytes.end());
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

    /* A delayed-format close can advance the sequence after self_sequence_ was
       sampled. The owner window remains ours, and is the authoritative second
       signal that this update must not echo back across the channel. */
    if (clipboard_update_is_external(
            sequence, self_sequence_, reinterpret_cast<uintptr_t>(GetClipboardOwner()),
            reinterpret_cast<uintptr_t>(window_))) {
        if (callbacks_.local_replaced) callbacks_.local_replaced();
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

/*
 * The files on the clipboard, as a list and a way to read them later.
 *
 * Directories are skipped rather than walked. A folder is a tree, and the
 * offer's metadata is a flat list of names with no room for the paths inside
 * one — so carrying a folder would need a wire change, not a loop here.
 * Skipped visibly, because a copied folder that silently transfers nothing is
 * the kind of quiet failure #42 exists to avoid.
 *
 * Called with the clipboard already open. It does **not** invoke the callback:
 * the caller closes the clipboard first, because that callback runs the whole
 * offer path — sealing, framing, the transport — and the clipboard is a
 * machine-wide lock that every other application is waiting on.
 */
bool Clipboard::read_files(std::vector<FileEntry> &entries,
                           std::function<bool(std::vector<uint8_t> &)> &read) {
    HANDLE handle = GetClipboardData(CF_HDROP);
    if (handle == nullptr) return false;
    auto *drop = static_cast<HDROP>(handle);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);

    entries.clear();
    std::vector<std::pair<std::wstring, uint64_t>> readable;
    unsigned skipped = 0;
    for (UINT i = 0; i < count; i++) {
        wchar_t path[MAX_PATH]{};
        if (DragQueryFileW(drop, i, path, MAX_PATH) == 0) {
            skipped++;
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) ||
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            skipped++;
            continue;
        }
        const uint64_t size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) |
                              attributes.nFileSizeLow;
        const wchar_t *name = wcsrchr(path, L'\\');
        name = name != nullptr ? name + 1 : path;
        entries.push_back(FileEntry{wide_to_utf8_string(name), size});
        readable.emplace_back(path, size);
    }
    if (skipped > 0 && callbacks_.log)
        callbacks_.log(std::to_string(skipped) +
                       " copied item(s) were not ordinary files — a folder is not carried — "
                       "and were left out");
    if (entries.empty()) return false;

    read = [readable](std::vector<uint8_t> &payload) -> bool {
        payload.clear();
        for (const auto &file : readable) {
            HANDLE handle = CreateFileW(file.first.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return false;
            /*
             * Measured again, not assumed from the offer. Reading the promised
             * length out of a file that has *grown* since the copy would take
             * its first bytes and send them as the whole thing — a truncated
             * file presented as complete, which is the one outcome #56 names
             * as unacceptable. A file that shrank is caught by the read
             * falling short; only this catches one that grew.
             */
            LARGE_INTEGER now{};
            if (!GetFileSizeEx(handle, &now) ||
                static_cast<uint64_t>(now.QuadPart) != file.second) {
                CloseHandle(handle);
                return false;
            }
            const size_t at = payload.size();
            payload.resize(at + static_cast<size_t>(file.second));
            uint64_t left = file.second;
            uint8_t *out = payload.data() + at;
            bool ok = true;
            while (left > 0) {
                const DWORD ask = left > 0x100000u ? 0x100000u : static_cast<DWORD>(left);
                DWORD got = 0;
                if (!ReadFile(handle, out, ask, &got, nullptr) || got == 0) {
                    ok = false;
                    break;
                }
                out += got;
                left -= got;
            }
            CloseHandle(handle);
            /* Short is not "nearly": the offer promised this length, so a file
               edited since the copy fails the transfer rather than truncating
               it. */
            if (!ok) return false;
        }
        return true;
    };
    return true;
}

void Clipboard::read_clipboard() {
    if (!callbacks_.local_copy && !callbacks_.local_image && !callbacks_.local_files) return;

    /* Ask first so unrelated clipboard formats cost nothing and do not open
       the clipboard against another program that wants it. */
    const bool has_text = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    const bool has_png = png_format_ != 0 && IsClipboardFormatAvailable(png_format_) != FALSE;
    const bool has_dibv5 = IsClipboardFormatAvailable(CF_DIBV5) != FALSE;
    const bool has_dib = IsClipboardFormatAvailable(CF_DIB) != FALSE;
    const bool has_bitmap = gdiplus_token_ != 0 && IsClipboardFormatAvailable(CF_BITMAP) != FALSE;
    const ClipboardImageFormat image_format =
        select_clipboard_image_format(has_png, has_dibv5, has_dib, has_bitmap);
    const bool has_image = image_format != ClipboardImageFormat::None;

    /*
     * Image first, then files, then text — the same order as the macOS twin,
     * and a divergence here is a clipboard that behaves differently on each
     * computer.
     *
     * **Files before text**, because copying one in Explorer also puts its
     * path on the clipboard as text, and reading text first would send the
     * path instead of the file.
     *
     * **An image before files**, because a screenshot tool writes its capture
     * to a temporary file and puts *both* on the clipboard — the image and a
     * path to it. Reading files first sent the screenshot as a file, so it
     * pasted into Explorer as a .png and would not paste into an image editor
     * at all, which is what a screenshot is for.
     */
    if (!has_image && callbacks_.local_files && IsClipboardFormatAvailable(CF_HDROP)) {
        if (!open_with_retry()) return;
        std::vector<FileEntry> files;
        std::function<bool(std::vector<uint8_t> &)> read;
        const bool found = read_files(files, read);
        CloseClipboard();
        if (found) {
            callbacks_.local_files(std::move(files), std::move(read));
            return;
        }
    }
    if (!has_text && !has_image) return;
    if (!open_with_retry()) return;

    std::vector<uint8_t> payload;
    const char *captured_format = nullptr;
    switch (image_format) {
    case ClipboardImageFormat::Png:
        payload = capture_clipboard_image(image_format, global_bytes(GetClipboardData(png_format_)));
        captured_format = "registered PNG";
        break;
    case ClipboardImageFormat::DibV5:
        payload = capture_clipboard_image(image_format, global_bytes(GetClipboardData(CF_DIBV5)));
        captured_format = "CF_DIBV5";
        break;
    case ClipboardImageFormat::Dib:
        payload = capture_clipboard_image(image_format, global_bytes(GetClipboardData(CF_DIB)));
        captured_format = "CF_DIB";
        break;
    case ClipboardImageFormat::Bitmap:
        if (HBITMAP bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)))
            payload = bitmap_to_png(bitmap);
        captured_format = "CF_BITMAP";
        break;
    case ClipboardImageFormat::None:
        break;
    }
    if (!payload.empty() && captured_format != nullptr && callbacks_.log)
        callbacks_.log(std::string("captured image from ") + captured_format);
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

/*
 * References to files that arrived, as CF_HDROP: a DROPFILES header followed
 * by a double-null-terminated list of wide paths.
 *
 * Host-only has no equivalent here and needs none — Windows has no Universal
 * Clipboard of its own to leak onto, which is why only the macOS side carries
 * that flag.
 */
bool Clipboard::deliver_files(const std::vector<std::wstring> &paths) {
    if (paths.empty()) return false;

    size_t characters = 1; /* the second terminator */
    for (const std::wstring &path : paths) characters += path.size() + 1;
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + characters * sizeof(wchar_t));
    if (block == nullptr) {
        if (callbacks_.log) callbacks_.log("a file list could not be allocated");
        return false;
    }
    auto *drop = static_cast<DROPFILES *>(GlobalLock(block));
    if (drop == nullptr) {
        GlobalFree(block);
        return false;
    }
    *drop = DROPFILES{};
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    auto *out = reinterpret_cast<wchar_t *>(reinterpret_cast<uint8_t *>(drop) + sizeof(DROPFILES));
    for (const std::wstring &path : paths) {
        std::memcpy(out, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
        out += path.size() + 1;
    }
    *out = L'\0';
    GlobalUnlock(block);

    if (!open_with_retry()) {
        GlobalFree(block);
        if (callbacks_.log)
            callbacks_.log("the clipboard would not open for " + std::to_string(paths.size()) +
                           " arriving file(s); they are on disk but cannot be pasted");
        return false;
    }
    EmptyClipboard();
    const bool wrote = SetClipboardData(CF_HDROP, block) != nullptr;
    /* Ownership passes to the clipboard only on success. */
    if (!wrote) GlobalFree(block);
    self_sequence_ = GetClipboardSequenceNumber();
    CloseClipboard();
    if (!wrote && callbacks_.log)
        callbacks_.log("the clipboard refused a list of " + std::to_string(paths.size()) +
                       " arriving file(s)");
    return wrote;
}

bool Clipboard::deliver_image(const std::vector<uint8_t> &png,
                              std::optional<DWORD> expected_sequence) {
    const ClipboardImageRepresentations representations = clipboard_image_representations(png);
    HGLOBAL png_block = png_format_ != 0 ? bytes_to_global(representations.png) : nullptr;
    HGLOBAL dibv5_block = bytes_to_global(representations.dibv5);
    HGLOBAL dib_block = bytes_to_global(representations.dib);
    if (png_block == nullptr || dibv5_block == nullptr || dib_block == nullptr) {
        if (png_block) GlobalFree(png_block);
        if (dibv5_block) GlobalFree(dibv5_block);
        if (dib_block) GlobalFree(dib_block);
        if (callbacks_.log)
            callbacks_.log("an arriving image could not build PNG, CF_DIBV5, and CF_DIB; nothing was written");
        return false;
    }
    if (!open_with_retry()) {
        GlobalFree(png_block);
        if (dibv5_block) GlobalFree(dibv5_block);
        if (dib_block) GlobalFree(dib_block);
        if (callbacks_.log) callbacks_.log("the image arrived but the clipboard would not open; it was not written");
        return false;
    }
    if (expected_sequence &&
        !prefetched_image_is_current(*expected_sequence, GetClipboardSequenceNumber())) {
        CloseClipboard();
        GlobalFree(png_block);
        if (dibv5_block) GlobalFree(dibv5_block);
        if (dib_block) GlobalFree(dib_block);
        if (callbacks_.log)
            callbacks_.log("a prefetched image was discarded because a newer Windows copy "
                           "exists");
        return false;
    }
    EmptyClipboard();
    const bool png_written = SetClipboardData(png_format_, png_block) != nullptr;
    if (!png_written) {
        GlobalFree(png_block);
        if (dibv5_block) GlobalFree(dibv5_block);
        if (dib_block) GlobalFree(dib_block);
        CloseClipboard();
        if (callbacks_.log) callbacks_.log("the clipboard refused an arriving image; it is now empty");
        return false;
    }
    const bool dibv5_written =
        dibv5_block != nullptr && SetClipboardData(CF_DIBV5, dibv5_block) != nullptr;
    if (dibv5_block != nullptr && !dibv5_written) GlobalFree(dibv5_block);
    const bool dib_written = dib_block != nullptr && SetClipboardData(CF_DIB, dib_block) != nullptr;
    if (dib_block != nullptr && !dib_written) GlobalFree(dib_block);
    if (callbacks_.log)
        callbacks_.log("published arriving image as registered PNG (exact bytes), CF_DIBV5=" +
                       std::string(dibv5_written ? "yes" : "no") + " CF_DIB=" +
                       std::string(dib_written ? "yes" : "no") +
                       " CF_BITMAP=no (no demonstrated compatibility need)");
    self_sequence_ = GetClipboardSequenceNumber();
    CloseClipboard();
    return dibv5_written && dib_written;
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
    /* A real handle, never nullptr: this claims no format it cannot
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
