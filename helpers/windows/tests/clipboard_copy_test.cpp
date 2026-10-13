/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

/* What one local copy sends, decided from a snapshot alone (#328). */

#include <cstdio>
#include <string>
#include <vector>

#include "clipboard_copy.h"

using namespace deskhop;

namespace {

/* Records which callback ran, and every log line, in order. */
struct Sent {
    std::string what;
    std::vector<uint8_t> bytes;
    std::vector<std::string> log;
};

ClipboardCallbacks recorder(Sent &sent) {
    ClipboardCallbacks callbacks;
    callbacks.local_copy = [&](std::vector<uint8_t> b) { sent.what = "text"; sent.bytes = b; };
    callbacks.local_image = [&](std::vector<uint8_t> b) { sent.what = "image"; sent.bytes = b; };
    callbacks.local_bundle = [&](std::vector<uint8_t> b) { sent.what = "bundle"; sent.bytes = b; };
    callbacks.local_files = [&](std::vector<FileEntry>, std::function<bool(std::vector<uint8_t> &)>) {
        sent.what = "files";
    };
    callbacks.log = [&](const std::string &line) { sent.log.push_back(line); };
    return callbacks;
}

std::vector<uint8_t> bytes(const char *text) {
    return std::vector<uint8_t>(text, text + std::char_traits<char>::length(text));
}

Sent act(ClipboardCopy copy) {
    Sent sent;
    act_on_copy(std::move(copy), recorder(sent), 100);
    return sent;
}

} // namespace

int main() {
    int failures = 0;
#define CHECK(condition, message)                    \
    do {                                             \
        if (!(condition)) {                          \
            ++failures;                              \
            std::printf("FAIL %s\n", (message));    \
        }                                            \
    } while (0)

    {
        /* Explorer puts a copied file's path beside it as text. */
        ClipboardCopy copy;
        copy.has_text = true;
        copy.files = {{"report.docx", 10}};
        CHECK(act(copy).what == "files", "a copied file's path was sent in place of the file");
    }
    {
        /* A screenshot tool: the capture beside its own temporary .png. */
        ClipboardCopy copy;
        copy.has_image = true;
        copy.image = bytes("png");
        copy.files = {{"Screenshot 1.PNG", 10}};
        CHECK(act(copy).what == "image", "an image beside its image file was sent as a file");
    }
    {
        /* A preview drawn beside a file that is not a picture. */
        ClipboardCopy copy;
        copy.has_image = true;
        copy.image = bytes("png");
        copy.files = {{"shot.png", 10}, {"notes.txt", 5}};
        CHECK(act(copy).what == "files", "a preview was sent in place of a non-image file");
        CHECK(clipboard_files_win(copy), "the read would capture an image the act then drops");
    }
    {
        ClipboardCopy copy;
        copy.has_text = copy.has_image = true;
        copy.text = bytes("hello");
        copy.image = bytes("png");
        const Sent sent = act(copy);
        CHECK(sent.what == "bundle", "text and a small picture did not travel as a bundle");
        CHECK(!sent.log.empty() && sent.log.back() ==
                  "bundling 5 bytes of text with its 3-byte picture",
              "the bundle log line changed");
    }
    {
        ClipboardCopy copy;
        copy.has_text = copy.has_image = true;
        copy.text = bytes("hello");
        copy.image = std::vector<uint8_t>(100, 1);
        const Sent sent = act(copy);
        CHECK(sent.what == "text" && sent.bytes == bytes("hello"),
              "text beside an over-limit picture was not sent alone");
        CHECK(sent.log.size() == 1 &&
                  sent.log[0] == "sending 5 bytes of text alone; the 100-byte picture beside it "
                                 "is over the bundle limit and does not travel",
              "the text-alone log line changed");
    }
    {
        ClipboardCopy copy;
        copy.has_image = true;
        copy.image = bytes("png");
        CHECK(act(copy).what == "image", "a picture alone was not sent as an image");
    }
    {
        /* The read's own lines come first, then the act's. */
        ClipboardCopy copy;
        copy.has_text = true;
        copy.notes = {"captured image from CF_DIB"};
        const Sent sent = act(copy);
        CHECK(sent.what.empty(), "an empty read sent something");
        CHECK(sent.log.size() == 2 && sent.log[0] == "captured image from CF_DIB" &&
                  sent.log[1] == "the clipboard offered content and then read as empty; "
                                 "nothing was sent",
              "an empty read was not logged after the read's own lines");
    }
    {
        /* Nothing on offer (a folder alone, or a failed open): silent. */
        ClipboardCopy copy;
        copy.notes = {"another program held the clipboard open through 5 attempts"};
        const Sent sent = act(copy);
        CHECK(sent.what.empty() && sent.log.size() == 1, "nothing on offer was not silent");
    }

    if (failures == 0) std::printf("clipboard_copy: all passed\n");
    return failures == 0 ? 0 : 1;
}
