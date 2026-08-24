#pragma once
/*
 * The helper's side of the session — as a **binding onto the shared core's
 * machine** (`dh_helper`, #79/#80), not a machine of its own.
 *
 * Nothing here decides anything. The hello exchange, negotiation, ADR-0004's
 * liveness, pairing, all-or-nothing acquisition, the capped backoff and the
 * states a user is shown are all src/core/dh_helper.c — the same object code
 * the firmware compiles and the macOS helper drives. This file carries events
 * in, carries outputs back, and owns the two halves the core deliberately
 * refuses: the *wording* (words.h) and the platform's key storage.
 *
 * The macOS twin is HelperSession.swift. Two of these existing is the point:
 * two *machines* would have given ADR-0004's traffic-gated liveness two
 * chances to be got right, failing differently on two operating systems under
 * load, in a way that looks like a hardware fault.
 *
 * Single-threaded by construction, like the rest of the helper — the core
 * assumes one caller and there is nothing here to lock.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dh_helper.h"

namespace deskhop {

/* This helper's key pair. The private half is 32 bytes the platform holds; on
   macOS the same seam wraps a Secure Enclave key that cannot be handed to C at
   all, which is why the core asks for one ECDH rather than for a key. */
struct Identity {
    std::vector<uint8_t> public_key; /* 64 raw bytes, X || Y, big-endian */
    std::vector<uint8_t> key_id;     /* SHA-256(public)[0..8] */

    /* ECDH against the board's public key — the raw 32-byte X coordinate of
       the agreed point. False when the board's key is not a point on the
       curve; the core reports that as one unusable device rather than as a
       frame that would not encode. */
    std::function<bool(const uint8_t *peer_public, uint8_t *shared_out)> ecdh;
};

/* What the core produced. One-for-one with dh_helper_output_kind, translated
   at the seam so the run loop never reads a raw tag. */
struct Output {
    enum class Kind { StoreBoardKey, OpenChannels, CloseChannels, Send, State, Retry, Note };

    Kind kind{Kind::Note};
    dh_helper_state state{DH_HELPER_QUIET};
    uint32_t retry_after_ms{0};
    std::vector<uint8_t> bytes; /* StoreBoardKey: the board's key. Send: a frame. */
    std::string note;           /* Note: already in words. */
};

class HelperSession {
  public:
    /* `board_public_key` is what the platform had stored, empty for a helper
       that has never paired. `entropy` must fill exactly the length it is
       asked for: it feeds nonces and correlation values, so a short draw would
       leave the core keying on bytes nobody chose. */
    HelperSession(Identity identity, const std::vector<uint8_t> &board_public_key,
                  std::function<void(uint8_t *out, size_t len)> entropy);

    /* The inputs, one per core entry point. Each returns what the core
       produced, in order. `now_ms` is a monotonic millisecond clock. */
    std::vector<Output> device_appeared(dh_device_identity which, uint32_t now_ms);
    std::vector<Output> device_disappeared(uint32_t now_ms);
    std::vector<Output> channels_acquired(uint8_t count, uint32_t now_ms);
    std::vector<Output> acquisition_refused(uint8_t acquired, uint8_t of, uint32_t now_ms);
    std::vector<Output> received(const uint8_t *data, size_t len, uint32_t now_ms);
    /* The transport could not carry something it was given. A frame written in
       part leaves the device's reader mid-frame, where the padding skip does
       not apply and the next frame is eaten as its tail — so this is a dropped
       connection, not a retryable write. The reason is for the log only: the
       core takes the failure, not the sentence. */
    std::vector<Output> transport_failed(const std::string &reason, uint32_t now_ms);
    std::vector<Output> tick(uint32_t now_ms);

    dh_helper_state state() const { return machine_->state; }
    bool have_negotiated() const { return machine_->have_negotiated; }
    dh_helper_negotiated negotiated() const { return machine_->negotiated; }

    /* Whether a bulk transfer may go out right now — the seam #52 consumes. It
       answers for the *session*, where words.h's allows_bulk answers for what
       the user is being told; the two must not disagree, so both call the
       core. */
    bool can_send_bulk() const { return dh_helper_can_send_bulk(machine_.get()); }

  private:
    std::vector<Output> collect(const std::string &transport_reason);

    /*
     * Held with stable addresses: `dh_helper_identity.ctx` is a bare pointer
     * the core calls through for the life of the machine, so neither this nor
     * the callbacks' backing store may move.
     */
    struct Callbacks {
        Identity identity;
        std::function<void(uint8_t *out, size_t len)> entropy;
    };

    std::unique_ptr<Callbacks> callbacks_;
    std::unique_ptr<dh_helper_identity> identity_;
    std::unique_ptr<dh_helper> machine_;
    std::unique_ptr<dh_helper_outputs> outputs_;
};

} // namespace deskhop
