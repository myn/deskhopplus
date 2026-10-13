/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include "clipboard.h"

#include <cstring>
#include <cwchar>
#include <memory>
#include <propidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <objidl.h>

#include "clipboard_update.h"
#include "clipboard_image.h"
#include "file_read.h"

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

/* The program that owns the clipboard, as its exe's file name (#330): the one
   a slow read waits on, so the one to close. Its pid when the name is out of
   reach. */
std::string clipboard_owner_name() {
    HWND owner = GetClipboardOwner();
    if (owner == nullptr) return "no program (the clipboard has no owner)";
    DWORD pid = 0;
    GetWindowThreadProcessId(owner, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    const bool named = process != nullptr && QueryFullProcessImageNameW(process, 0, path, &size);
    if (process != nullptr) CloseHandle(process);
    if (!named) return "pid " + std::to_string(pid);
    const wchar_t *slash = std::wcsrchr(path, L'\\');
    return wide_to_utf8_string(slash != nullptr ? slash + 1 : path);
}

uint32_t clock_ms() { return static_cast<uint32_t>(GetTickCount64()); }

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

/* NUL-terminated wide text in a movable block, as CF_UNICODETEXT wants it.
   Null when it cannot be allocated or locked. */
HGLOBAL wide_to_global(const std::wstring &wide) {
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (block == nullptr) return nullptr;
    auto *destination = static_cast<wchar_t *>(GlobalLock(block));
    if (destination == nullptr) {
        GlobalFree(block);
        return nullptr;
    }
    std::memcpy(destination, wide.c_str(), bytes);
    GlobalUnlock(block);
    return block;
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

using Log = std::function<void(const std::string &)>;

/* OpenClipboard with the bounded retry. `window` null opens it for the calling
   thread, which is how a read's thread opens it: it only reads. */
bool open_clipboard(HWND window, const Log &log) {
    DWORD delay = kFirstRetryMs;
    for (int attempt = 1; attempt <= kOpenAttempts; attempt++) {
        if (OpenClipboard(window)) {
            if (attempt > 1 && log)
                log("the clipboard took " + std::to_string(attempt) + " attempts to open");
            return true;
        }
        if (attempt < kOpenAttempts) {
            Sleep(delay);
            delay *= 2;
        }
    }
    if (log)
        log("another program held the clipboard open through " + std::to_string(kOpenAttempts) +
            " attempts");
    return false;
}

/* What a read's thread needs, copied off the Clipboard when it starts (#329). */
struct ReadSource {
    UINT png_format;
    bool gdiplus;
    bool want_files;
    Log log; /* only for the files' read closure, which runs on accept */
};

ClipboardCopy read_copy(const ReadSource &source); /* below, beside the writes */

struct ReadJob {
    ReadSource source;
    HWND reply;
};

DWORD WINAPI read_thread(void *parameter) {
    std::unique_ptr<ReadJob> job(static_cast<ReadJob *>(parameter));
    /* The image paths make IStreams; the window's thread has COM, this one
       needs its own. */
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto *copy = new ClipboardCopy(read_copy(job->source));
    if (SUCCEEDED(com)) CoUninitialize();
    if (!PostMessageW(job->reply, Clipboard::kReadDoneMessage, 0, reinterpret_cast<LPARAM>(copy)))
        delete copy;
    return 0;
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
    reads_ = ClipboardReads(self_sequence_);
    listening_ = AddClipboardFormatListener(window) != FALSE;
    if (!listening_ && callbacks_.log)
        callbacks_.log("the clipboard listener could not be registered; copies made on this "
                       "computer will not be sent");
}


void Clipboard::detach() {
    if (listening_ && window_ != nullptr) RemoveClipboardFormatListener(window_);
    listening_ = false;
    window_ = nullptr;
    /* A read still in flight may be inside GDI+ (a CF_BITMAP copy), and may
       never end: its source app can wait for ever. GDI+ is left up then, and
       the process exit that follows a detach takes it down. */
    if (gdiplus_token_ != 0 && !reads_.reading()) Gdiplus::GdiplusShutdown(gdiplus_token_);
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

bool Clipboard::handle(UINT message, WPARAM parameter, LPARAM detail) {
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
    if (message == kReadDoneMessage) {
        std::unique_ptr<ClipboardCopy> copy(reinterpret_cast<ClipboardCopy *>(detail));
        const std::function<void()> held = reads_.read_done(copy->sequence);
        if (slow_read_.end() && callbacks_.log)
            callbacks_.log("the slow clipboard read ended after " +
                           std::to_string(slow_read_.waited(clock_ms())) + " ms");
        /* Decided on what the read found open, not on the update that started
           it: a write of this helper's that landed before the read opened the
           clipboard is what it took, and must not echo back. */
        if (clipboard_update_is_external(copy->sequence, self_sequence_, copy->owner,
                                         reinterpret_cast<uintptr_t>(window_)))
            act_on_copy(std::move(*copy), callbacks_, ClipService::kEagerImageThreshold);
        /* An arriving write held while the read had the clipboard. Its own
           self_sequence_ keeps the update it causes from reading it back. */
        if (held) held();
        /* A copy made while that read waited started nothing; read it now. */
        on_update();
        return true;
    }
    if (message != WM_CLIPBOARDUPDATE) return false;
    on_update();
    return true;
}

void Clipboard::on_update() {
    /*
     * The message is *posted*, so what it names is "something changed", not
     * which change. By the time it is handled the sequence number is whatever
     * the clipboard holds now — so two messages queued behind one change both
     * read the same clipboard, and without `reads_` the same payload crosses
     * the link twice.
     */
    const DWORD sequence = GetClipboardSequenceNumber();
    if (trace_lazy_lifecycle_ && callbacks_.log)
        callbacks_.log("[clipboard-debug] WM_CLIPBOARDUPDATE sequence=" +
                       std::to_string(sequence) + " handled=" +
                       std::to_string(reads_.handled()) + " self=" +
                       std::to_string(self_sequence_) + " owner=" +
                       std::to_string(reinterpret_cast<uintptr_t>(GetClipboardOwner())) +
                       " helper=" + std::to_string(reinterpret_cast<uintptr_t>(window_)) +
                       " has_text=" +
                       std::to_string(IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE) +
                       " has_png=" +
                       std::to_string(png_format_ != 0 &&
                                      IsClipboardFormatAvailable(png_format_) != FALSE) +
                       " has_bitmap=" +
                       std::to_string(IsClipboardFormatAvailable(CF_BITMAP) != FALSE) +
                       " reading=" + std::to_string(reads_.reading()));

    /* A delayed-format close can advance the sequence after self_sequence_ was
       sampled. The owner window remains ours, and is the authoritative second
       signal that this update must not echo back across the channel. */
    const bool external = clipboard_update_is_external(
        sequence, self_sequence_, reinterpret_cast<uintptr_t>(GetClipboardOwner()),
        reinterpret_cast<uintptr_t>(window_));
    if (!reads_.update(sequence, external)) return;
    if (callbacks_.local_replaced) callbacks_.local_replaced();
    /* The first external update is the replacement under investigation.
       One line records it; ordinary copies after it stay off the helper's
       synchronously flushed diagnostic path. */
    trace_lazy_lifecycle_ = false;
    start_read();
}

void Clipboard::start_read() {
    if (!callbacks_.local_copy && !callbacks_.local_image && !callbacks_.local_files) {
        reads_.read_done(0);
        return;
    }
    slow_read_.start(clock_ms());
    /* Plain copies, so the thread shares nothing with this one. */
    auto *job = new ReadJob{ReadSource{png_format_, gdiplus_token_ != 0,
                                       static_cast<bool>(callbacks_.local_files), callbacks_.log},
                            window_};
    HANDLE thread = CreateThread(nullptr, 0, read_thread, job, 0, nullptr);
    if (thread != nullptr) {
        /* Detached, never joined: a read can wait on its source app for ever,
           and a quit must not wait with it. Its reply to a window already gone
           fails and frees the copy; the process exit ends the thread. */
        CloseHandle(thread);
        return;
    }
    /* No thread: read here, as before #329, rather than drop the copy. */
    if (callbacks_.log) callbacks_.log("a clipboard read thread could not start; reading inline");
    auto *copy = new ClipboardCopy(read_copy(job->source));
    delete job;
    handle(kReadDoneMessage, 0, reinterpret_cast<LPARAM>(copy));
}

void Clipboard::tick(uint32_t now) {
    /* The owner is looked up only now: the read holds the clipboard open, so
       it is still the one the read started on. */
    if (slow_read_.warn(now) && callbacks_.log)
        callbacks_.log("a clipboard read has waited " + std::to_string(slow_read_.waited(now)) +
                       " ms on " + clipboard_owner_name() + ", which owns the clipboard");
}

void Clipboard::claim_lazy_image(uint32_t id, uint64_t total) {
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

/* While a read holds the clipboard, each of these is held and runs at its end
   (#329): the latest only, since each replaces the clipboard anyway. */
void Clipboard::deliver_text(const std::vector<uint8_t> &utf8) {
    reads_.write([this, utf8] { write_text(utf8); });
}
void Clipboard::deliver_image(const std::vector<uint8_t> &png,
                              std::optional<DWORD> expected_sequence) {
    reads_.write([this, png, expected_sequence] { write_image(png, expected_sequence); });
}
void Clipboard::deliver_bundle(const std::vector<uint8_t> &utf8,
                               const std::vector<uint8_t> &png) {
    reads_.write([this, utf8, png] { write_bundle(utf8, png); });
}
void Clipboard::deliver_files(const std::vector<std::wstring> &paths) {
    reads_.write([this, paths] { write_files(paths); });
}
void Clipboard::lazy_image(uint32_t id, uint64_t total) {
    reads_.write([this, id, total] { claim_lazy_image(id, total); });
}

bool Clipboard::open_with_retry() {
    return window_ != nullptr && open_clipboard(window_, callbacks_.log);
}

namespace {

/*
 * The files on the clipboard, as a list and a way to read them later.
 *
 * Directories are skipped rather than walked. A folder is a tree, and the
 * offer's metadata is a flat list of names with no room for the paths inside
 * one — so carrying a folder would need a wire change, not a loop here.
 * Skipped visibly, because a copied folder that silently transfers nothing is
 * the kind of quiet failure #42 exists to avoid.
 *
 * Called with the clipboard already open. It invokes no callback: the offer
 * path — sealing, framing, the transport — runs only after the clipboard is
 * closed, because the clipboard is a machine-wide lock that every other
 * application is waiting on.
 */
bool read_files(ClipboardCopy &copy, const ReadSource &source) {
    HANDLE handle = GetClipboardData(CF_HDROP);
    if (handle == nullptr) return false;
    auto *drop = static_cast<HDROP>(handle);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);

    std::vector<FileEntry> &entries = copy.files;
    entries.clear();
    std::vector<std::pair<std::wstring, FileEntry>> readable;
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
        readable.emplace_back(path, entries.back());
    }
    if (skipped > 0)
        copy.notes.push_back(std::to_string(skipped) +
                             " copied item(s) were not ordinary files — a folder is not "
                             "carried — and were left out");
    if (entries.empty()) return false;

    copy.read_files = [readable, log = source.log](std::vector<uint8_t> &payload) -> bool {
        payload.clear();
        for (const auto &[path, file] : readable) {
            /* Each file at its offered length (#182). The name and not the
               path goes in the log: the offer already carries the name. */
            const FileRead result = read_offered(path, file.size, payload);
            std::string line;
            switch (result.outcome) {
            case FileRead::Outcome::Read:
                if (result.size_now > file.size)
                    line = file.name + " grew by " + std::to_string(result.size_now - file.size) +
                           " bytes since the copy; the offered length was sent";
                break;
            case FileRead::Outcome::Shrank:
                line = file.name + " is " + std::to_string(result.size_now) + " bytes now and " +
                       std::to_string(file.size) +
                       " were offered; it shrank since the copy, so the transfer was abandoned";
                break;
            case FileRead::Outcome::OpenFailed:
                line = file.name + ": open failed, error " + std::to_string(result.error);
                break;
            case FileRead::Outcome::ReadFailed:
                line = file.name + ": read failed, error " + std::to_string(result.error);
                break;
            }
            if (log && !line.empty()) log(line);
            if (result.outcome != FileRead::Outcome::Read) return false;
        }
        return true;
    };
    return true;
}

/* Take the copy off the clipboard (#328). Calls nothing, the log included:
   its lines go in the snapshot's notes. Runs on a read's thread (#329), so it
   touches only `source` and opens the clipboard with no window. */
ClipboardCopy read_copy(const ReadSource &source) {
    ClipboardCopy copy;

    /* Ask first so unrelated clipboard formats cost nothing and do not open
       the clipboard against another program that wants it. */
    const bool has_text = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    const bool has_png =
        source.png_format != 0 && IsClipboardFormatAvailable(source.png_format) != FALSE;
    const bool has_dibv5 = IsClipboardFormatAvailable(CF_DIBV5) != FALSE;
    const bool has_dib = IsClipboardFormatAvailable(CF_DIB) != FALSE;
    const bool has_bitmap = source.gdiplus && IsClipboardFormatAvailable(CF_BITMAP) != FALSE;
    const ClipboardImageFormat image_format =
        select_clipboard_image_format(has_png, has_dibv5, has_dib, has_bitmap);
    const bool has_image = image_format != ClipboardImageFormat::None;
    const bool has_files = source.want_files && IsClipboardFormatAvailable(CF_HDROP);

    /* One open for files, text and image. The has_ flags stay false until it
       succeeds, so a copy that could not be read sends nothing and says only
       why (`act_on_copy`). */
    if (!has_files && !has_text && !has_image) return copy;
    if (!open_clipboard(nullptr, [&copy](const std::string &line) { copy.notes.push_back(line); }))
        return copy;
    copy.sequence = GetClipboardSequenceNumber();
    copy.owner = reinterpret_cast<uintptr_t>(GetClipboardOwner());
    copy.has_text = has_text;
    copy.has_image = has_image;
    const bool files_found = has_files && read_files(copy, source);
    /* Files that will be sent need nothing else read: see `clipboard_files_win`. */
    if (clipboard_files_win(copy)) {
        CloseClipboard();
        return copy;
    }

    /*
     * Both read when both are there. Every Office application puts a picture
     * of a text selection beside the text, and until #195 the picture won and
     * the text was dropped — so text copied in PowerPoint pasted on the Mac as
     * a picture of the words. Which of the two goes, or whether both go
     * together, is `select_clipboard_send`'s call once the sizes are known.
     */
    std::vector<uint8_t> &image = copy.image;
    const char *captured_format = nullptr;
    switch (image_format) {
    case ClipboardImageFormat::Png:
        image = capture_clipboard_image(image_format,
                                        global_bytes(GetClipboardData(source.png_format)));
        captured_format = "registered PNG";
        break;
    case ClipboardImageFormat::DibV5:
        image = capture_clipboard_image(image_format, global_bytes(GetClipboardData(CF_DIBV5)));
        captured_format = "CF_DIBV5";
        break;
    case ClipboardImageFormat::Dib:
        image = capture_clipboard_image(image_format, global_bytes(GetClipboardData(CF_DIB)));
        captured_format = "CF_DIB";
        break;
    case ClipboardImageFormat::Bitmap:
        if (HBITMAP bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)))
            image = bitmap_to_png(bitmap);
        captured_format = "CF_BITMAP";
        break;
    case ClipboardImageFormat::None:
        break;
    }
    if (!image.empty() && captured_format != nullptr)
        copy.notes.push_back(std::string("captured image from ") + captured_format);
    /* Not read beside files, even the image-only files a screenshot tool
       leaves: the text beside a copied file is its path, and a bundle of a
       path and a picture would paste the path into a text field. Gated on a
       file being *found*, as the macOS twin is, so a copied folder still
       behaves as it did. */
    if (has_text && !files_found) {
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (const auto *wide = static_cast<const wchar_t *>(GlobalLock(handle))) {
            copy.text = wide_to_utf8(wide);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return copy;
}

} // namespace

/*
 * References to files that arrived, as CF_HDROP: a DROPFILES header followed
 * by a double-null-terminated list of wide paths.
 *
 * Host-only has no equivalent here and needs none — Windows has no Universal
 * Clipboard of its own to leak onto, which is why only the macOS side carries
 * that flag.
 */
bool Clipboard::write_files(const std::vector<std::wstring> &paths) {
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
    if (callbacks_.log)
        callbacks_.log(wrote ? std::to_string(paths.size()) +
                                   " file(s) written and put on the clipboard"
                             : "the clipboard refused a list of " +
                                   std::to_string(paths.size()) + " arriving file(s)");
    return wrote;
}

bool Clipboard::write_image(const std::vector<uint8_t> &png,
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

void Clipboard::write_bundle(const std::vector<uint8_t> &utf8,
                             const std::vector<uint8_t> &png) {
    /* Every handle is built before the clipboard is opened, so nothing else is
       kept waiting on this process's allocator or its image codec. */
    const std::wstring wide = utf8_to_wide(utf8.data(), utf8.size());
    HGLOBAL text_block = wide.empty() ? nullptr : wide_to_global(wide);
    const ClipboardImageRepresentations representations = clipboard_image_representations(png);
    HGLOBAL png_block = png_format_ != 0 ? bytes_to_global(representations.png) : nullptr;
    HGLOBAL dibv5_block = bytes_to_global(representations.dibv5);
    HGLOBAL dib_block = bytes_to_global(representations.dib);
    HGLOBAL blocks[] = {text_block, png_block, dibv5_block, dib_block};
    const auto free_all = [&blocks] {
        for (HGLOBAL &block : blocks)
            if (block) { GlobalFree(block); block = nullptr; }
    };
    /* A part that will not build costs that part, not the bundle: the text is
       what the copy was about (#195), and the picture is the part the copy
       side already drops when it must. The single-format writer takes over,
       as it does for a bundle that arrived with one usable part. */
    if (png_block == nullptr || dibv5_block == nullptr || dib_block == nullptr) {
        free_all();
        if (callbacks_.log)
            callbacks_.log("an arriving bundle's picture could not build PNG, CF_DIBV5, and "
                           "CF_DIB; its text is written alone");
        write_text(utf8);
        return;
    }
    if (text_block == nullptr) {
        free_all();
        if (callbacks_.log)
            callbacks_.log("an arriving bundle's text would not convert to wide text; its "
                           "picture is written alone");
        write_image(png, std::nullopt);
        return;
    }
    if (!open_with_retry()) {
        free_all();
        if (callbacks_.log)
            callbacks_.log("a bundle arrived but the clipboard would not open; it was not written");
        return;
    }
    EmptyClipboard();
    /* Ownership passes to the clipboard only on success; a refused handle is
       still ours to free. Written as one transaction between one open and one
       close, which is what makes the text and the picture one clipboard
       entry rather than two. */
    const UINT formats[] = {CF_UNICODETEXT, png_format_, CF_DIBV5, CF_DIB};
    bool written[4] = {};
    for (size_t i = 0; i < 4; i++) {
        written[i] = SetClipboardData(formats[i], blocks[i]) != nullptr;
        if (!written[i]) GlobalFree(blocks[i]);
    }
    self_sequence_ = GetClipboardSequenceNumber();
    CloseClipboard();
    if (callbacks_.log)
        callbacks_.log(std::string("published arriving bundle: CF_UNICODETEXT=") +
                       (written[0] ? "yes" : "no") + " PNG=" + (written[1] ? "yes" : "no") +
                       " CF_DIBV5=" + (written[2] ? "yes" : "no") + " CF_DIB=" +
                       (written[3] ? "yes" : "no"));
}

void Clipboard::write_text(const std::vector<uint8_t> &utf8) {
    const std::wstring wide = utf8_to_wide(utf8.data(), utf8.size());
    if (wide.empty() && !utf8.empty()) {
        if (callbacks_.log)
            callbacks_.log("a payload of " + std::to_string(utf8.size()) +
                           " bytes would not convert to wide text; nothing was written");
        return;
    }

    /* Allocated before the clipboard is opened, so nothing else is kept
       waiting on this process's allocator. */
    HGLOBAL block = wide_to_global(wide);
    if (block == nullptr) {
        if (callbacks_.log) callbacks_.log("no memory for an arriving clipboard payload");
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
