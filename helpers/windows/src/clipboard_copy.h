/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#pragma once
/*
 * One local copy in two steps (#328): `Clipboard` reads the clipboard into a
 * `ClipboardCopy` and calls nothing; `act_on_copy` decides what that copy
 * sends and calls the callbacks. No Win32 here, so the decision tests on any
 * machine, and the read alone can move off the window's thread (#326).
 */

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "clip_service.h"
#include "clipboard_update.h"
#include "dh_bundle.h"

namespace deskhop {

struct ClipboardCallbacks {
    /* Text copied on this computer, already filtered for this helper's own
       writes. UTF-8. */
    std::function<void(std::vector<uint8_t>)> local_copy;
    std::function<void(std::vector<uint8_t>)> local_image;
    /* Text and its picture together (#195), already packed as a kind-3
       payload — one transfer, so the pasting application picks. */
    std::function<void(std::vector<uint8_t>)> local_bundle;
    /* Any external sequence, before inspecting its formats or send policy. */
    std::function<void()> local_replaced;
    std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint64_t)> request_image;
    std::function<void(uint32_t)> lazy_image_replaced;
    /* Files copied here (#56): what they are called and how long they are,
       plus the callable that reads them — invoked only if the transfer is
       accepted on the other computer. */
    std::function<void(std::vector<FileEntry>,
                       std::function<bool(std::vector<uint8_t> &)>)> local_files;
    std::function<void(const std::string &)> log;
};

/* Everything one read took off the clipboard. A size of zero is "not there". */
struct ClipboardCopy {
    /* Log lines the read would have written, in order. The act writes them
       first, so a copy logs exactly what it did before the split. */
    std::vector<std::string> notes;
    /* What the clipboard offered, before any of it was read. */
    bool has_text{false};
    bool has_image{false};
    /* Ordinary files found, and the closure that reads them on accept. */
    std::vector<FileEntry> files;
    std::function<bool(std::vector<uint8_t> &)> read_files;
    std::vector<uint8_t> text;
    std::vector<uint8_t> image;
};

/*
 * Whether every copied file is itself a picture — the one case where image data
 * on the clipboard beside a file path is the thing the user meant, rather than
 * a preview something drew for a file that is not an image.
 *
 * Extension-based on purpose: the question is what the user copied, and asking
 * the shell for a type means loading the file, which a clipboard poll must not
 * do.
 */
inline bool all_files_are_images(const std::vector<FileEntry> &files) {
    static const char *const kImageExtensions[] = {".png",  ".jpg",  ".jpeg", ".gif", ".bmp",
                                                   ".tif",  ".tiff", ".webp", ".heic"};
    if (files.empty()) return false;
    for (const FileEntry &file : files) {
        const size_t dot = file.name.rfind('.');
        if (dot == std::string::npos) return false;
        std::string ext = file.name.substr(dot);
        for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool matched = false;
        for (const char *candidate : kImageExtensions)
            if (ext == candidate) { matched = true; break; }
        if (!matched) return false;
    }
    return true;
}

/*
 * Files first; then text and image together as a bundle when both are there
 * and small enough, else whichever is there (#195). The same order as the
 * macOS twin, and a divergence here is a clipboard that behaves differently on
 * each computer.
 *
 * **Files before text**, because copying one in Explorer also puts its path on
 * the clipboard as text, and reading text first would send the path instead of
 * the file.
 *
 * **An image before files only when the files *are* images.** A screenshot
 * tool writes its capture to a temporary file and puts both on the clipboard,
 * and sending that as a file made it paste into Explorer as a .png instead of
 * into an image editor — so an image still wins there.
 *
 * The old rule read the image first unconditionally, on the claim that image
 * data beside a path is the screenshot case and nothing else. That claim is
 * wrong wherever anything puts a preview or an icon on the clipboard beside a
 * copied file: the preview is then sent in place of the file. It cost the
 * macOS twin every single-file copy (#56), and the rule being shared is the
 * reason to correct both.
 *
 * Also asked by the read, which then skips the text and image it would drop.
 */
inline bool clipboard_files_win(const ClipboardCopy &copy) {
    return !copy.files.empty() && !(copy.has_image && all_files_are_images(copy.files));
}

/* What one copy sends, through the callbacks. No clipboard call: the snapshot
   is all it reads. `bundle_limit` is ClipService::kEagerImageThreshold. */
inline void act_on_copy(ClipboardCopy copy, const ClipboardCallbacks &callbacks,
                        size_t bundle_limit) {
    if (callbacks.log)
        for (const std::string &line : copy.notes) callbacks.log(line);

    if (clipboard_files_win(copy)) {
        if (callbacks.local_files)
            callbacks.local_files(std::move(copy.files), std::move(copy.read_files));
        return;
    }
    if (!copy.has_text && !copy.has_image) return;

    std::vector<uint8_t> &text = copy.text;
    std::vector<uint8_t> &image = copy.image;
    switch (select_clipboard_send(text.size(), image.size(), bundle_limit)) {
    case ClipboardSend::Nothing:
        /*
         * An empty read is not the same as an empty clipboard. The managed
         * laptop runs Trellix DLP, whose block behaviour at the Win32 level is
         * undocumented, so a read that "succeeded" and returned nothing cannot
         * be ruled out (#60) — the spec's standing instruction is to treat a
         * clipboard read failure as an expected state and say so rather than
         * trust it. Nothing is sent, and the reason is in the log.
         */
        if (callbacks.log)
            callbacks.log("the clipboard offered content and then read as empty; nothing was sent");
        return;
    case ClipboardSend::Bundle: {
        const dh_bundle_part parts[] = {
            {DH_BUNDLE_PART_TEXT, text.data(), static_cast<uint32_t>(text.size())},
            {DH_BUNDLE_PART_PNG, image.data(), static_cast<uint32_t>(image.size())},
        };
        std::vector<uint8_t> packed(dh_bundle_packed_len(parts, 2));
        if (dh_bundle_pack(parts, 2, packed.data(), packed.size()) == 0) {
            if (callbacks.log) callbacks.log("text and its picture would not pack; nothing was sent");
            return;
        }
        if (callbacks.log)
            callbacks.log("bundling " + std::to_string(text.size()) + " bytes of text with its " +
                          std::to_string(image.size()) + "-byte picture");
        if (callbacks.local_bundle) callbacks.local_bundle(std::move(packed));
        return;
    }
    case ClipboardSend::Image:
        if (callbacks.local_image) callbacks.local_image(std::move(image));
        return;
    case ClipboardSend::Text:
        if (!image.empty() && callbacks.log)
            callbacks.log("sending " + std::to_string(text.size()) +
                          " bytes of text alone; the " + std::to_string(image.size()) +
                          "-byte picture beside it is over the bundle limit and does not travel");
        if (callbacks.local_copy) callbacks.local_copy(std::move(text));
        return;
    }
}

} // namespace deskhop
