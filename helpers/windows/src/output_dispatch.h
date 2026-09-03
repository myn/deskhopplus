#pragma once
/*
 * What each output *does* — the shim's two switches, with the platform behind
 * an interface (#152).
 *
 * Both services below it are covered in depth: `HelperSession` binds the
 * shared core, `ClipService` joins the seal to the transfer, and both have
 * their own suites. The layer that turns each of their outputs into a real
 * effect had none, so every arm was correct by reading only — a service could
 * be proved to emit the right output while the shim dropped it, called the
 * wrong thing, or fell through. That is the shape of #93 and #94: a helper
 * that reports healthy while doing nothing.
 *
 * No Win32 here, which is the point. The tray, the HID transport, the
 * clipboard and the secret store stay in main.cpp behind `HelperEffects`, and
 * this file only decides which of them an output reaches. The wording is
 * words.h's, as everywhere else.
 *
 * The macOS twin is OutputDispatch.swift, and the two are deliberately the
 * same shape — a divergence between them is a clipboard that works on one
 * computer and not the other. One arm is not the same, and by decision: the
 * State arm here checks `words::state_is_known` and drives a tray, and macOS
 * does neither *here*. It makes the same check one layer up — HelperSession
 * turns a state its wording has no case for into a note before it becomes an
 * output at all — and it has no notification-area icon to update.
 *
 * Single-threaded by construction, like the rest of the helper.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "clip_service.h"
#include "helper_session.h"

namespace deskhop {

/*
 * Every effect an output can have, named once.
 *
 * main.cpp implements each of these in a line or two over the real object —
 * the tray, the transport, this computer's clipboard — and a test implements
 * them by writing down what it was asked to do. Nothing in this interface
 * takes a Win32 type, so the dispatch below builds and runs anywhere.
 */
class HelperEffects {
  public:
    virtual ~HelperEffects() = default;

    /* SecretStore: false when the key could not be written. */
    virtual bool store_board_key(const std::vector<uint8_t> &key) = 0;

    /* HidTransport. `send` is false when the transport would not take the
       frame, which is a different reading from a quiet link (#107, #132). */
    virtual void acquire_channels() = 0;
    virtual void release_channels() = 0;
    virtual bool send(const std::vector<uint8_t> &frame) = 0;

    /* HelperSession. The counter space belongs to the session key, so a frame
       is built there and never here. */
    virtual bool build_frame(uint8_t type, const std::vector<uint8_t> &body,
                             std::vector<uint8_t> &out) = 0;
    virtual void note_sent() = 0;
    virtual void note_send_refused() = 0;

    /* Tray. */
    virtual void show_state(dh_helper_state state) = 0;

    /* This computer's clipboard. */
    virtual void deliver_text(const std::vector<uint8_t> &utf8) = 0;
    virtual void deliver_image(const std::vector<uint8_t> &png) = 0;
    virtual void lazy_image(uint32_t id, uint64_t total) = 0;
    virtual void cancel_lazy_image(uint32_t id) = 0;
    /* Files (#56). `ask_about_files` puts the acceptance to the user: nothing
       has crossed the link yet, and nothing will until the user answers. */
    virtual void ask_about_files(const deskhop::FileOffer &offer) = 0;
    virtual void withdraw_file_question(uint32_t id) = 0;
    virtual void deliver_files(const FileDelivery &delivery) = 0;

    /* The run loop's retry timer. The clock and its wrap-safe arithmetic stay
       with the loop that owns them. */
    virtual void schedule_retry(uint32_t after_ms) = 0;

    /* ClipService. The board is the single source of truth for the policy and
       the size cap, so a direction turning off — or a cap moving — has to reach
       the service that honours it (#52, #56). */
    virtual std::vector<ClipOutput> clip_policy_changed(uint8_t flags, uint8_t cap_mb) = 0;

    virtual void log(const std::string &message) = 0;
};

class OutputDispatch {
  public:
    /* `effects` must outlive this object. */
    explicit OutputDispatch(HelperEffects &effects) : effects_(effects) {}

    /* The session's outputs — the core's, through HelperSession. */
    void apply(const Output &output);
    void apply(const std::vector<Output> &outputs);

    /* One clipboard output, and a batch of them. */
    void emit(const ClipOutput &output);
    void emit(const std::vector<ClipOutput> &outputs);

  private:
    HelperEffects &effects_;
};

} // namespace deskhop
