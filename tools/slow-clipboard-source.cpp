// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Derek Reynolds

// Slow clipboard source: reproduces #326 on demand (#327).
//
// It copies a line of text with delayed rendering: it puts CF_UNICODETEXT on
// the clipboard with no data, and when an app (the helper, Notepad) asks for
// the text, it waits before it gives it. That is a slow app, like the VS Code
// that froze the helper.
//
// Build on the Mac (mingw-w64):
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -o slow-clipboard-source.exe tools/slow-clipboard-source.cpp
// Test the argument parsing on the Mac:
//   c++ -std=c++17 -DSELF_TEST -o /tmp/scs-test tools/slow-clipboard-source.cpp && /tmp/scs-test
//
// Run on Windows, in a console, with the helper running:
//   slow-clipboard-source.exe [seconds]      (default 40)
// The copy fires a clipboard update, so the helper reads the text and waits.
// Try to paste in Notepad during the wait. The tool exits after it gives the
// text; Ctrl+C exits sooner.
//
// Expected with a helper that reads the clipboard on its beat thread: the
// helper log goes quiet, the board ends the session for liveness, and the
// log says "in the ~Nms before that the board's USB took 1 report(s)".
// The ~30 s stalls in #326 suggest Windows stops a reader's wait at about
// 30 s; if so, N is about 30000 for any wait of 30 s or more. A shorter wait
// (say 10) shows whether N follows it.

#include <cstdio>
#include <cstdlib>

// Reads the wait, in ms, from the command line: no argument means 40 s, one
// argument is whole seconds from 1 to 3600. False for anything else.
static bool parse_wait_ms(int argc, char **argv, unsigned *wait_ms) {
    if (argc == 1) {
        *wait_ms = 40000;
        return true;
    }
    if (argc != 2)
        return false;
    char *end;
    long s = std::strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || s < 1 || s > 3600)
        return false;
    *wait_ms = (unsigned)s * 1000;
    return true;
}

#ifdef SELF_TEST
#include <cassert>

int main() {
    unsigned ms = 0;
    char prog[] = "scs", ten[] = "10", zero[] = "0", junk[] = "10s", big[] = "3601";
    char *none[] = {prog}, *a10[] = {prog, ten}, *a0[] = {prog, zero},
         *aj[] = {prog, junk}, *ab[] = {prog, big}, *two[] = {prog, ten, ten};
    assert(parse_wait_ms(1, none, &ms) && ms == 40000);
    assert(parse_wait_ms(2, a10, &ms) && ms == 10000);
    assert(!parse_wait_ms(2, a0, &ms));
    assert(!parse_wait_ms(2, aj, &ms));
    assert(!parse_wait_ms(2, ab, &ms));
    assert(!parse_wait_ms(3, two, &ms));
    std::puts("parse_wait_ms: ok");
    return 0;
}

#else
#include <windows.h>

static unsigned g_wait_ms;
static const wchar_t kText[] = L"deskhopplus slow clipboard source (#327)";

static void log_line(const char *what) {
    std::printf("%10lums  %s\n", GetTickCount(), what);
    std::fflush(stdout);
}

// Puts the text on the clipboard. The caller has it open or is answering
// WM_RENDERFORMAT, where Windows has it open for the asking app.
static void give_text() {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof kText);
    if (!h)
        return;
    memcpy(GlobalLock(h), kText, sizeof kText);
    GlobalUnlock(h);
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        GlobalFree(h);
        log_line("SetClipboardData failed: the asking app gave up waiting");
    } else {
        log_line("text given");
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_RENDERFORMAT: {
        char line[64];
        std::snprintf(line, sizeof line, "an app asked for the text; waiting %u ms", g_wait_ms);
        log_line(line);
        Sleep(g_wait_ms);
        give_text();
        PostQuitMessage(0);
        return 0;
    }
    case WM_RENDERALLFORMATS:
        // We are exiting with the text still owed: give it now, no wait.
        if (OpenClipboard(hwnd)) {
            if (GetClipboardOwner() == hwnd)
                give_text();
            CloseClipboard();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char **argv) {
    if (!parse_wait_ms(argc, argv, &g_wait_ms)) {
        std::fprintf(stderr, "usage: slow-clipboard-source [seconds 1..3600, default 40]\n");
        return 2;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SlowClipboardSource";
    RegisterClassW(&wc);
    // A hidden top-level window, not HWND_MESSAGE: it owns the clipboard.
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (!hwnd || !OpenClipboard(hwnd)) {
        std::fprintf(stderr, "cannot open the clipboard (error %lu)\n", GetLastError());
        return 1;
    }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, nullptr); // delayed rendering
    CloseClipboard();
    log_line("copied with delayed rendering; waiting for an app to ask");

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DestroyWindow(hwnd);
    return 0;
}
#endif
