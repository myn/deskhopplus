/*
 * The clipboard payload path (#52, #55), driven as **two helpers talking to each
 * other** rather than one talking to a script.
 *
 * That is the whole value of the shape. Every rule these exercise — the seal
 * exchange, the credit window, the received-set, abandon-on-drop — is the
 * shared core's, and a test where one end answers a mock of the other would be
 * checking the mock. Here a copy made on one side is carried, frame by frame,
 * through a relay that does nothing but hand bytes over, and the check is that
 * the bytes come out the far side identical.
 *
 * The board is not modelled, deliberately. Between two helpers it is an opaque
 * relay that holds no key and reads no payload (ADR-0008), so a hop that hands
 * bytes over unchanged is exactly what one is.
 *
 * Windows only, because the cipher is: ClipService takes the platform's AEAD
 * and this suite gives it the one the helper actually ships. The macOS twin is
 * helpers/macos/Tests/channel-tests/ClipboardTests.swift, and the two check the
 * same claims — that is the point of both existing.
 *
 * Style follows seal_test.cpp — an assertion macro, a main, a printed failure
 * line, a non-zero exit. No framework (ADR-0006).
 */

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <set>

#include "clip_service.h"
#include "dh_file_list.h"
#include "file_naming.h"
#include "dh_session.h"
#include "seal_aead.h"

using deskhop::ClipKind;
using deskhop::ClipOutput;
using deskhop::ClipService;
using deskhop::seal_aead;

static int failures = 0;

#define CHECK(cond, what)                                               \
    do {                                                                \
        if (!(cond)) {                                                  \
            ++failures;                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, (what)); \
        }                                                               \
    } while (0)

namespace {

std::vector<uint8_t> bytes_of(const std::string &text) {
    std::vector<uint8_t> out(text.size());
    for (size_t i = 0; i < text.size(); i++) out[i] = static_cast<uint8_t>(text[i]);
    return out;
}

std::string text_of(const std::vector<uint8_t> &bytes) {
    std::string out(bytes.size(), '\0');
    for (size_t i = 0; i < bytes.size(); i++) out[i] = static_cast<char>(bytes[i]);
    return out;
}

/*
 * Entropy that is deterministic but not constant: a seal needs a fresh
 * ephemeral key and nonce per exchange, and a source that returned the same
 * bytes twice would key both directions identically and hide a real mix-up.
 */
std::function<void(uint8_t *, size_t)> counter_entropy(uint8_t seed) {
    /* uint8_t{0}, not 0: a bare literal is an int, and MSVC at /W4 reports the
       narrowing inside make_shared's instantiation — where /WX makes it an
       error. Every conversion in this file is spelled out for that reason. */
    auto step = std::make_shared<uint8_t>(uint8_t{0});
    return [seed, step](uint8_t *out, size_t len) {
        ++*step;
        for (size_t i = 0; i < len; i++)
            out[i] = static_cast<uint8_t>(i * 31u + seed * 7u + *step);
    };
}

enum class Side { A, B };

/* Two helpers and the link between them. */
struct Pair {
    explicit Pair(size_t capacity = 64u * 1024u)
        : capacity(capacity), a(seal_aead(), counter_entropy(1), capacity),
          b(seal_aead(), counter_entropy(2), capacity) {}

    size_t capacity;
    ClipService a;
    ClipService b;
    std::vector<std::vector<uint8_t>> delivered_to_a;
    std::vector<std::vector<uint8_t>> delivered_to_b;
    std::vector<std::string> notes;
    /* Frames carried across the link, so a test can say what a direction cost. */
    size_t carried = 0;
    size_t lazy_images = 0;
    uint32_t last_lazy_image_id = 0;
    bool request_lazy_images = true;
    /*
     * The paste-side acceptance (#56). Every file offer that reaches a side
     * is recorded, and — unless a test says otherwise — answered the way a user
     * clicking Accept would. A test that wants the *held* state, which is the
     * whole point of the gate, sets `answer_file_offers` to false and the offer
     * stays waiting.
     */
    std::vector<deskhop::FileOffer> file_questions;
    std::vector<uint32_t> withdrawn_questions;
    bool answer_file_offers = true;
    bool accept_file_offers = true;
    std::vector<deskhop::FileDelivery> files_to_a;
    std::vector<deskhop::FileDelivery> files_to_b;
    /* Every frame put on the link, in order, so a test can hand one over again
       later — the only way to reach a message that was sealed under a key its
       receiver has since replaced. */
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> carried_frames;
    /*
     * Frames the link loses before they reach the far end, counted down by
     * message type. This is the seam ADR-0005 describes — a bounded queue
     * refusing a frame with no retransmit beneath it — and it is the only way
     * to reach the failure #145 reports, because on a healthy link nothing is
     * ever lost.
     */
    std::map<uint8_t, int> drop_next;

    /*
     * Carry every frame to its far end, and everything that answers, until the
     * two ends have nothing left to say. Bounded: a pair that will not settle
     * is a defect, and a test that hangs reports it as a timeout an hour later.
     */
    void settle(std::vector<ClipOutput> outputs, Side origin) {
        std::vector<std::pair<Side, ClipOutput>> queue;
        for (ClipOutput &item : outputs) queue.emplace_back(origin, std::move(item));

        size_t rounds = 0;
        while (!queue.empty()) {
            if (++rounds > 100000) {
                CHECK(false, "the two helpers never stopped answering each other");
                return;
            }
            const std::pair<Side, ClipOutput> entry = std::move(queue.front());
            queue.erase(queue.begin());

            const Side side = entry.first;
            const ClipOutput &output = entry.second;
            switch (output.kind) {
            case ClipOutput::Kind::Send: {
                carried++;
                carried_frames.emplace_back(output.type, output.bytes);
                /* A frame going out is a chance to push the next credit-gated
                   batch, which is what the run loop does on the same seam — and
                   it happens whether or not the link carries this one. */
                ClipService &near = side == Side::A ? a : b;
                for (ClipOutput &item : near.pump()) queue.emplace_back(side, std::move(item));
                auto lost = drop_next.find(output.type);
                if (lost != drop_next.end() && lost->second > 0) {
                    lost->second--; /* refused at a seam with no retransmit beneath it */
                    break;
                }
                ClipService &far = side == Side::A ? b : a;
                const Side far_side = side == Side::A ? Side::B : Side::A;
                for (ClipOutput &item :
                     far.received(output.type, output.bytes.data(), output.bytes.size()))
                    queue.emplace_back(far_side, std::move(item));
                break;
            }
            case ClipOutput::Kind::Deliver:
                (side == Side::A ? delivered_to_a : delivered_to_b).push_back(output.bytes);
                break;
            case ClipOutput::Kind::LazyImage: {
                lazy_images++;
                last_lazy_image_id = output.transfer_id;
                if (!request_lazy_images) break;
                ClipService &near = side == Side::A ? a : b;
                for (ClipOutput &item : near.request_lazy_image(output.transfer_id))
                    queue.emplace_back(side, std::move(item));
                break;
            }
            case ClipOutput::Kind::CancelLazyImage:
                break;
            case ClipOutput::Kind::FileOffer: {
                file_questions.push_back(
                    deskhop::FileOffer{output.transfer_id, output.total, output.files});
                if (!answer_file_offers) break;
                ClipService &near = side == Side::A ? a : b;
                for (ClipOutput &item : accept_file_offers
                                            ? near.accept_files(output.transfer_id)
                                            : near.decline_files(output.transfer_id))
                    queue.emplace_back(side, std::move(item));
                break;
            }
            case ClipOutput::Kind::FileOfferWithdrawn:
                withdrawn_questions.push_back(output.transfer_id);
                break;
            case ClipOutput::Kind::DeliverFiles:
                (side == Side::A ? files_to_a : files_to_b)
                    .push_back(deskhop::FileDelivery{output.files, output.bytes});
                break;
            case ClipOutput::Kind::Note:
                notes.push_back(output.note);
                break;
            case ClipOutput::Kind::ProtocolError:
                notes.push_back("protocol error: " + output.note);
                break;
            }
        }
    }

    /*
     * A's helper process went away and came back: a new service, with an
     * offer-id namespace that starts at one again. B is untouched, which is the
     * whole asymmetry — its session never ended, and no call on a living
     * service can reproduce this (#151).
     */
    void restart_a() { a = ClipService(seal_aead(), counter_entropy(3), capacity); }

    void copy_on_a(const std::string &text) {
        settle(a.local_copy(ClipKind::Text, bytes_of(text)), Side::A);
    }

    /* Files copied on A, with a provider that hands over `bytes` — the read
       that must not happen until the far side accepts.

       A bare `int *` into the caller's local is safe here and is not in the
       Swift twin, which needs an object: a C++ local has a stable address for
       its whole scope, and the provider is called before this returns. Swift's
       `&x` produces a pointer valid only for the call it is passed to, so the
       same shape there wrote to freed stack in a release build — and passed in
       debug. */
    void copy_files_on_a(const std::vector<deskhop::FileEntry> &files,
                         const std::vector<uint8_t> &bytes, int *reads = nullptr) {
        settle(a.local_copy_files(files,
                                  [bytes, reads](std::vector<uint8_t> &out) {
                                      if (reads != nullptr) (*reads)++;
                                      out = bytes;
                                      return true;
                                  }),
               Side::A);
    }

    void copy_on_b(const std::string &text) {
        settle(b.local_copy(ClipKind::Text, bytes_of(text)), Side::B);
    }

    bool saw_note(const std::string &fragment) const {
        for (const std::string &note : notes)
            if (note.find(fragment) != std::string::npos) return true;
        return false;
    }
};

// ------------------------------------------------------------------ the path

void test_text_crosses_the_link() {
    Pair pair;
    pair.copy_on_a("hello from Windows");

    CHECK(pair.delivered_to_b.size() == 1, "text copied on A did not arrive on B");
    if (pair.delivered_to_b.size() == 1)
        CHECK(text_of(pair.delivered_to_b[0]) == "hello from Windows",
              "the text that arrived is not the text that was copied");
    CHECK(pair.delivered_to_a.empty(), "A was handed its own copy back");

    /* And the other way, on the same pair: the two directions are independent
       and each has its own seal. */
    pair.copy_on_b("hello from the Mac");
    CHECK(pair.delivered_to_a.size() == 1, "text copied on B did not arrive on A");
    if (pair.delivered_to_a.size() == 1)
        CHECK(text_of(pair.delivered_to_a[0]) == "hello from the Mac",
              "the reverse direction carried the wrong text");
}

void test_image_crosses_the_link() {
    for (size_t size : {size_t{1024}, ClipService::kEagerImageThreshold + 1}) {
        std::vector<uint8_t> png(size, 0x55);
        Pair pair(png.size() + 1024u);
        pair.settle(pair.a.local_copy(ClipKind::Png, png), Side::A);
        CHECK(pair.delivered_to_b.size() == 1, "the image did not arrive");
        if (pair.delivered_to_b.size() == 1)
            CHECK(pair.delivered_to_b[0] == png,
                  "the eager/lazy image was not byte-identical end to end");
        CHECK(pair.lazy_images == (size > ClipService::kEagerImageThreshold ? 1u : 0u),
              "the image did not take the threshold-selected path");
    }
}

void test_a_lazy_offer_retry_does_not_reclaim_the_clipboard() {
    std::vector<uint8_t> png(ClipService::kEagerImageThreshold + 1, 0x55);
    Pair pair(png.size() + 1024u);
    pair.request_lazy_images = false;
    pair.settle(pair.a.local_copy(ClipKind::Png, png), Side::A);
    CHECK(pair.lazy_images == 1, "the first lazy offer did not claim the clipboard once");
    CHECK(pair.saw_note("[clipboard-debug] lazy offer id="),
          "the initial lazy offer was not identified in diagnostics");

    (void)pair.a.tick(0);
    pair.settle(pair.a.tick(ClipService::kSweepDelayMs), Side::A);
    CHECK(pair.lazy_images == 1,
          "an idempotent offer retry reclaimed and erased the clipboard");
    CHECK(pair.saw_note("disposition=duplicate"),
          "the retried lazy offer was not identified as a duplicate");
}

void test_replacing_a_lazy_image_cancels_its_receive() {
    std::vector<uint8_t> png(ClipService::kEagerImageThreshold + 1, 0x55);
    Pair pair(png.size() + 1024u);
    pair.request_lazy_images = false;
    pair.settle(pair.a.local_copy(ClipKind::Png, png), Side::A);
    CHECK(pair.last_lazy_image_id != 0, "the lazy image never claimed the clipboard");

    pair.settle(pair.b.lazy_image_was_replaced(pair.last_lazy_image_id), Side::B);
    CHECK(pair.b.request_lazy_image(pair.last_lazy_image_id).empty(),
          "a replaced lazy image could still start receiving");
    CHECK(pair.delivered_to_b.empty(),
          "a replaced lazy image was delivered over the newer local copy");
}

/*
 * ADR-0003: the channel is fidelity-preserving. The wire payload is
 * byte-identical end to end and nothing on the path validates, filters or
 * normalizes it — so the bytes that must survive are the awkward ones.
 */
void test_fidelity_is_preserved() {
    std::string awkward = "tab\there\nnewline, ";
    awkward += "\xE2\x80\x94"; /* em dash */
    awkward += " and a NUL: ";
    awkward.push_back('\0');
    awkward += " after it";

    Pair pair;
    pair.copy_on_a(awkward);
    CHECK(pair.delivered_to_b.size() == 1, "the awkward payload did not arrive");
    if (pair.delivered_to_b.size() == 1)
        CHECK(pair.delivered_to_b[0] == bytes_of(awkward),
              "the payload was not byte-identical end to end");
}

/*
 * A payload never goes out unsealed, and the exchange is a round trip — so the
 * first copy after a session begins waits for a key rather than travelling
 * without one.
 */
void test_nothing_leaves_unsealed() {
    ClipService a(seal_aead(), counter_entropy(1));
    const std::vector<ClipOutput> first = a.local_copy(ClipKind::Text, bytes_of("waiting"));

    /* Counted as frames, not as outputs: the copy also says out loud that it is
       waiting, and a note is not on the wire. */
    std::vector<uint8_t> frames;
    for (const ClipOutput &item : first)
        if (item.kind == ClipOutput::Kind::Send) frames.push_back(item.type);
    CHECK(frames.size() == 1 && frames[0] == DH_MSG_SEAL_OFFER,
          "a copy with no seal put something other than one seal offer on the wire");
}

void test_a_lost_seal_offer_is_retried() {
    Pair pair;
    pair.drop_next[DH_MSG_SEAL_OFFER] = 1;
    pair.copy_on_a("survives a lost seal offer");

    pair.a.tick(0);
    pair.settle(pair.a.tick(ClipService::kSweepDelayMs), Side::A);

    CHECK(pair.delivered_to_b.size() == 1,
          "a copy did not recover after its SEAL_OFFER was lost");
}

void test_a_lost_seal_accept_is_retried() {
    Pair pair;
    pair.drop_next[DH_MSG_SEAL_ACCEPT] = 1;
    pair.copy_on_a("survives a lost seal accept");

    pair.a.tick(0);
    pair.settle(pair.a.tick(ClipService::kSweepDelayMs), Side::A);

    CHECK(pair.delivered_to_b.size() == 1,
          "a copy did not recover after its SEAL_ACCEPT was lost");
}

void test_seal_retries_do_not_extend_the_copy_deadline() {
    Pair pair;
    pair.drop_next[DH_MSG_SEAL_OFFER] = 100;
    pair.copy_on_a("never sealed");

    pair.a.tick(0);
    for (uint32_t now = ClipService::kSweepDelayMs; now < ClipService::kStallTimeoutMs;
         now += ClipService::kSweepDelayMs)
        pair.settle(pair.a.tick(now), Side::A);
    pair.settle(pair.a.tick(ClipService::kStallTimeoutMs), Side::A);

    CHECK(pair.saw_note("waiting for a seal") && pair.saw_note("was abandoned"),
          "a copy whose seal exchange never completed was not reported abandoned");
    CHECK(pair.a.tick(ClipService::kStallTimeoutMs + ClipService::kSweepDelayMs).empty(),
          "a seal retry kept the terminal deadline open");
}

/*
 * One chunk is 1024 bytes and the credit window is 16, so a payload of a few
 * tens of kilobytes is the first one where credit, batching and the
 * received-set all actually run. Below that the machinery is present and never
 * exercised.
 */
void test_a_multi_chunk_payload_arrives() {
    std::string long_text;
    for (int i = 0; i < 4000; i++) long_text += "deskhopplus "; /* ~48 KB */

    Pair pair;
    pair.copy_on_a(long_text);
    CHECK(pair.delivered_to_b.size() == 1, "a multi-chunk payload did not arrive");
    if (pair.delivered_to_b.size() == 1)
        CHECK(pair.delivered_to_b[0] == bytes_of(long_text),
              "a multi-chunk payload was reassembled wrongly");
    CHECK(pair.carried > 40, "a 48 KB payload crossed in too few frames to have been chunked");
}

/* What the user last copied is what they mean to paste. */
void test_a_second_copy_supersedes() {
    Pair pair;
    std::vector<ClipOutput> outputs = pair.a.local_copy(ClipKind::Text, bytes_of("first"));
    for (ClipOutput &item : pair.a.local_copy(ClipKind::Text, bytes_of("second")))
        outputs.push_back(std::move(item));
    pair.settle(std::move(outputs), Side::A);

    CHECK(pair.delivered_to_b.size() == 1, "the wrong number of payloads arrived");
    if (pair.delivered_to_b.size() == 1)
        CHECK(text_of(pair.delivered_to_b[0]) == "second",
              "the superseded copy arrived, or the newer one did not");
}

// -------------------------------------------------------------- the two toggles

void test_sending_off_stops_one_direction() {
    Pair pair;
    /* A→B off: A may not send, B may not receive — the same fact told to each
       end in its own terms (dh_clip_policy_for). */
    pair.settle(pair.a.policy_changed(static_cast<uint8_t>(DH_CLIP_MAY_RECEIVE)), Side::A);
    pair.settle(pair.b.policy_changed(static_cast<uint8_t>(DH_CLIP_MAY_SEND)), Side::B);

    pair.copy_on_a("this must not cross");
    CHECK(pair.delivered_to_b.empty(), "a copy crossed a direction that is turned off");
    CHECK(pair.saw_note("clipboard sending turned off"),
          "nothing said why the copy did not cross");

    /* The other direction is untouched. This is the criterion a single global
       toggle would pass by accident. */
    pair.copy_on_b("this must still cross");
    CHECK(pair.delivered_to_a.size() == 1, "turning off one direction stopped the other");
}

/*
 * A helper told not to receive refuses the offer from its clear head, without
 * opening the seal. Decrypting a payload it has already decided to refuse would
 * be the one place a turned-off direction still handled the content.
 */
void test_receiving_off_refuses_the_offer() {
    Pair pair;
    pair.settle(pair.b.policy_changed(static_cast<uint8_t>(DH_CLIP_MAY_SEND)), Side::B);

    pair.copy_on_a("refused at the far end");
    CHECK(pair.delivered_to_b.empty(), "a payload was written to a clipboard that is off");
    CHECK(pair.saw_note("clipboard receiving turned off"),
          "the far end did not say why it refused");
    /* The sender is told, so a transfer does not sit half-open waiting. */
    CHECK(pair.saw_note("was abandoned"),
          "the sending end was never told its transfer was cancelled");
}

/*
 * Turning a toggle off mid-transfer takes what is crossing with it. Letting the
 * bytes already on the wire finish arriving would make the control a control
 * over *new* copies only, which is not what "stop content leaving this machine"
 * means.
 */
void test_a_turned_off_toggle_abandons_in_flight() {
    Pair pair;
    pair.copy_on_a("warm the seal");

    std::string long_text(40000, 'x');
    std::vector<ClipOutput> outputs = pair.a.local_copy(ClipKind::Text, bytes_of(long_text));
    for (ClipOutput &item : pair.a.policy_changed(static_cast<uint8_t>(DH_CLIP_MAY_RECEIVE)))
        outputs.push_back(std::move(item));
    pair.settle(std::move(outputs), Side::A);

    CHECK(pair.delivered_to_b.size() == 1,
          "a transfer survived the toggle that was meant to stop it");
    CHECK(pair.saw_note("sending was turned off"),
          "nothing recorded the toggle stopping a transfer");
}

// ------------------------------------------------------------ losing the session

void test_a_lost_session_abandons_everything() {
    Pair pair;
    pair.copy_on_a("before the drop");
    CHECK(pair.delivered_to_b.size() == 1, "the first copy did not arrive");

    const std::vector<ClipOutput> dropped = pair.a.session_ended();
    for (const ClipOutput &item : dropped)
        CHECK(item.kind != ClipOutput::Kind::Send,
              "a frame was produced for a session that had already gone");

    /* The next copy re-offers a seal rather than reusing the dead one. */
    const std::vector<ClipOutput> after =
        pair.a.local_copy(ClipKind::Text, bytes_of("after the drop"));
    CHECK(!after.empty(), "nothing was sent after the session came back");
    if (!after.empty())
        CHECK(after[0].kind == ClipOutput::Kind::Send && after[0].type == DH_MSG_SEAL_OFFER,
              "a copy after a lost session reused the dead seal");
}

/*
 * The ordinary recovery when one end's session ended and the other's did not:
 * the far helper holds no key for the seal it is sent, says so, and the sender
 * offers a fresh one with the payload still waiting behind it.
 */
void test_a_stale_seal_is_reoffered() {
    Pair pair;
    pair.copy_on_a("first, to establish a seal");
    CHECK(pair.delivered_to_b.size() == 1, "the first copy did not arrive");

    /* Only B's session ends. A still holds a seal B can no longer open. */
    (void)pair.b.session_ended();

    pair.copy_on_a("second, across a seal the far end has forgotten");
    CHECK(pair.delivered_to_b.size() == 2,
          "a payload was lost when the far end forgot the seal");
    if (pair.delivered_to_b.size() == 2)
        CHECK(text_of(pair.delivered_to_b[1]) == "second, across a seal the far end has forgotten",
              "the re-offered payload arrived wrong");
    CHECK(pair.saw_note("offering a fresh one"), "the fresh seal offer was not recorded");
}

/*
 * A control message that will not decode is refused rather than acted on with
 * whatever the fields happened to read as. These carry transfer ids and
 * sequence numbers, so a misread one cancels or re-requests the wrong thing.
 */
void test_malformed_control_messages() {
    ClipService a(seal_aead(), counter_entropy(1));
    const uint8_t junk[2] = {0x01, 0x02};
    const uint8_t types[] = {DH_MSG_CLIP_REQUEST, DH_MSG_CLIP_DONE, DH_MSG_CLIP_CANCEL,
                             DH_MSG_CLIP_RETRANSMIT, DH_MSG_CLIP_CREDIT};

    for (uint8_t type : types)
        for (const ClipOutput &output : a.received(type, junk, sizeof junk))
            CHECK(output.kind == ClipOutput::Kind::Note,
                  "a malformed control message produced something other than a diagnostic");

    /* A type this helper has no case for is named, not dropped silently. */
    const std::vector<ClipOutput> unknown = a.received(static_cast<uint8_t>(0x3F), nullptr, 0);
    CHECK(unknown.size() == 1 && unknown[0].kind == ClipOutput::Kind::Note,
          "an unknown clipboard message vanished without a word");
}

/*
 * A helper on the other computer may be gone, or it may have accepted the
 * payload and gone quiet. Without a delivery acknowledgement those states are
 * indistinguishable, so silence alone cannot end or diagnose the send.
 */
void test_a_stalled_send_is_retained() {
    ClipService a(seal_aead(), counter_entropy(1));
    ClipService b(seal_aead(), counter_entropy(2));

    /* Get as far as an offer sitting on the wire, then stop answering. */
    std::vector<ClipOutput> outputs = a.local_copy(ClipKind::Text, bytes_of("into the void"));
    for (size_t i = 0; i < outputs.size(); i++) {
        if (outputs[i].kind != ClipOutput::Kind::Send) continue;
        for (const ClipOutput &reply :
             b.received(outputs[i].type, outputs[i].bytes.data(), outputs[i].bytes.size())) {
            if (reply.kind != ClipOutput::Kind::Send) continue;
            for (ClipOutput &back : a.received(reply.type, reply.bytes.data(), reply.bytes.size()))
                outputs.push_back(std::move(back));
        }
    }

    /* There is no delivery acknowledgement in v2. Silence is equally
       consistent with success, so it cannot abandon or diagnose the send. */
    CHECK(a.tick(1).empty(), "a transfer was abandoned on its first tick");
    bool false_failure = false;
    for (const ClipOutput &output : a.tick(ClipService::kStallTimeoutMs * 3))
        if ((output.kind == ClipOutput::Kind::Note &&
             output.note.find("not answering") != std::string::npos) ||
            (output.kind == ClipOutput::Kind::Send && output.type == DH_MSG_CLIP_CANCEL))
            false_failure = true;
    CHECK(!false_failure, "an unanswered send was falsely diagnosed or abandoned");
}

/*
 * An abandonment quotes what the board says it has dropped (#133).
 *
 * A stall on a board that has been losing frames and a stall on a board with
 * clean seams are different faults, and until #133 the difference could not be
 * measured at all: the totals were readable only from the config page, which
 * is reachable only by rebooting the board that holds them.
 *
 * The three answers the line has to keep apart are "the board has said
 * nothing", "the board says nothing was dropped", and "the board says *this*
 * was dropped" — the first two being the pair that got read as each other.
 */
static std::string stall_note(const dh_device_drops *drops) {
    Pair pair;
    pair.copy_on_a("warm the seal");
    pair.drop_next[DH_MSG_CLIP_CHUNK] = 10000;
    pair.settle(pair.a.local_copy(ClipKind::Text, std::vector<uint8_t>(5000, 1)), Side::A);

    /* The first tick arms the stall clock; the second is past it. */
    pair.b.tick(1, drops);
    for (const ClipOutput &output : pair.b.tick(ClipService::kStallTimeoutMs + 1, drops))
        if (output.kind == ClipOutput::Kind::Note &&
            output.note.find("abandoned") != std::string::npos)
            return output.note;
    return std::string();
}

void test_a_stall_says_what_the_board_has_dropped() {
    CHECK(stall_note(nullptr).find("stated no drop totals") != std::string::npos,
          "a stall on a board that has said nothing did not say so");

    dh_device_drops clean{};
    CHECK(stall_note(&clean).find("no drops") != std::string::npos,
          "a stall on a board with clean seams did not say so");

    /* One seam, named — and only that one, so the line is all signal. */
    clean.truncated = 3;
    const std::string named = stall_note(&clean);
    CHECK(named.find("peer frames truncated 3") != std::string::npos,
          "a stall did not name the seam the board says is losing frames");
    CHECK(named.find("orphan") == std::string::npos,
          "a stall listed a seam that had lost nothing");

    /*
     * The outbound total is three causes in one number, and which of them is
     * moving decides what to do about it — a deeper bulk queue fixes nothing
     * if the single-frame priority band is what refused, and a bad header is
     * version skew rather than congestion at all (#142).
     *
     * The exact reading this was written for: the total climbing while the
     * bulk band, the one #141 deepened, is clean.
     */
    dh_device_drops split{};
    split.outq = 7;
    split.outq_priority = 7;
    CHECK(stall_note(&split).find("outbound refused 7 (priority 7, bulk 0, bad header 0)") !=
              std::string::npos,
          "a stall did not say which band of the outbound queue refused");
}

/*
 * The timeout measures *progress*, not the transfer's total duration. A large
 * payload legitimately takes minutes on this link, and a deadline on the whole
 * transfer would abandon healthy ones — which is why the counters exist rather
 * than a single start time.
 */
void test_progress_keeps_a_transfer_alive() {
    Pair pair;
    pair.copy_on_a("warm the seal");

    std::string long_text(40000, 'y');
    pair.settle(pair.a.local_copy(ClipKind::Text, bytes_of(long_text)), Side::A);
    CHECK(pair.delivered_to_b.size() == 2, "the long payload did not arrive");

    /* Ticks far beyond the timeout, on a pair that has been talking all along.
       Nothing is owed and nothing is running, so nothing is abandoned. */
    CHECK(pair.a.tick(ClipService::kStallTimeoutMs * 10).empty(),
          "a finished transfer was reported as stalled");
}

/*
 * A helper told not to receive never decrypts a payload it has already decided
 * to refuse (docs/protocol.md) — and that has to hold for chunks, not only for
 * the offer that introduced them. It is reachable in the ordinary way: turning
 * the toggle off mid-transfer cancels the transfer, but up to a credit window
 * of chunks is already in flight behind that cancel.
 *
 * What makes the refusal observable is the *silence*. A chunk that reached the
 * seal under a key this end does not hold would come back as a SEAL_STALE; one
 * refused ahead of it produces nothing at all.
 */
void test_chunks_are_refused_before_the_seal_is_opened() {
    ClipService a(seal_aead(), counter_entropy(1));
    ClipService b(seal_aead(), counter_entropy(2));

    /* A real sealed chunk, built by A. */
    std::vector<uint8_t> chunk;
    std::vector<ClipOutput> queue =
        a.local_copy(ClipKind::Text, bytes_of(std::string(4000, 'z')));
    for (size_t i = 0; i < queue.size() && chunk.empty() && i < 1000; i++) {
        if (queue[i].kind != ClipOutput::Kind::Send) continue;
        if (queue[i].type == DH_MSG_CLIP_CHUNK) {
            chunk = queue[i].bytes;
            break;
        }
        for (const ClipOutput &reply :
             b.received(queue[i].type, queue[i].bytes.data(), queue[i].bytes.size())) {
            if (reply.kind != ClipOutput::Kind::Send) continue;
            for (ClipOutput &back : a.received(reply.type, reply.bytes.data(), reply.bytes.size()))
                queue.push_back(std::move(back));
        }
        for (ClipOutput &more : a.pump()) queue.push_back(std::move(more));
    }
    CHECK(!chunk.empty(), "no sealed chunk was produced to test with");
    if (chunk.empty()) return;

    /* A third helper, holding no seal at all and told not to receive. Without
       the guard it would try to open the chunk, fail to find the seal, and
       answer SEAL_STALE — which is the payload having reached the cipher. */
    ClipService refusing(seal_aead(), counter_entropy(3));
    (void)refusing.policy_changed(static_cast<uint8_t>(DH_CLIP_MAY_SEND));
    CHECK(refusing.received(DH_MSG_CLIP_CHUNK, chunk.data(), chunk.size()).empty(),
          "a chunk reached the seal on a helper that had already refused to receive");

    /* And the control: with receiving allowed, the same chunk *does* reach the
       seal, so the check above is measuring the guard rather than nothing. */
    ClipService accepting(seal_aead(), counter_entropy(4));
    bool answered = false;
    for (const ClipOutput &output : accepting.received(DH_MSG_CLIP_CHUNK, chunk.data(),
                                                       chunk.size()))
        if (output.kind == ClipOutput::Kind::Send && output.type == DH_MSG_SEAL_STALE)
            answered = true;
    CHECK(answered,
          "with receiving on, a chunk under an unknown seal was not answered with SEAL_STALE");
}

/*
 * A receive the link starved gets itself going again (#145).
 *
 * Every credit grant the first chunks earn is lost. The sender stops at zero
 * credit and nothing message-driven can fire on either end: no chunk arrives
 * to prompt the receiver, and no CLIP_DONE arrives to drive the sweep that
 * CLIP_DONE used to be the only way to reach. Before this the transfer sat
 * there until the thirty-second deadline reported it lost — at no consistent
 * size and no consistent fraction, which is exactly what the log showed.
 */
void test_a_stalled_receive_asks_again() {
    Pair pair;
    pair.copy_on_a("warm the seal");

    pair.drop_next[DH_MSG_CLIP_CREDIT] = 6;
    const std::string long_text(40000, 'z');
    pair.settle(pair.a.local_copy(ClipKind::Text, bytes_of(long_text)), Side::A);
    CHECK(pair.delivered_to_b.size() == 1, "the transfer was expected to stall and did not");

    /* The receiving end's tick, at its own cadence and nothing else's. */
    uint32_t now = ClipService::kSweepDelayMs;
    while (pair.delivered_to_b.size() < 2 && now < ClipService::kStallTimeoutMs) {
        pair.settle(pair.b.tick(now), Side::B);
        now += ClipService::kSweepDelayMs;
    }
    CHECK(pair.delivered_to_b.size() == 2, "the starved receive never recovered");
    CHECK(pair.delivered_to_b.back() == bytes_of(long_text),
          "the recovered payload is not the one that was sent");
    CHECK(pair.saw_note("asked for again"),
          "the receive recovered without saying it had stalled");
}

/* #146: the copy side has reached DONE, but a lost chunk and lost early
   retransmit requests leave the paste side incomplete. A copy-side timeout
   must not discard the payload before a later receive sweep asks again. */
void test_a_copy_side_deadline_does_not_defeat_a_later_receive_sweep() {
    Pair pair;
    pair.copy_on_a("warm the seal");

    pair.drop_next[DH_MSG_CLIP_CHUNK] = 1;
    pair.drop_next[DH_MSG_CLIP_RETRANSMIT] = 10000;
    const std::string payload(45000, 'r');
    pair.settle(pair.a.local_copy(ClipKind::Text, bytes_of(payload)), Side::A);
    CHECK(pair.delivered_to_b.size() == 1,
          "the deliberately incomplete transfer unexpectedly arrived");

    pair.a.tick(0);
    pair.a.tick(ClipService::kStallTimeoutMs + 1);

    pair.drop_next.clear();
    pair.b.tick(0);
    pair.settle(pair.b.tick(ClipService::kSweepDelayMs + 1), Side::B);

    CHECK(pair.delivered_to_b.size() == 2,
          "the old copy-side deadline discarded the payload before the late retransmit");
    if (pair.delivered_to_b.size() == 2)
        CHECK(text_of(pair.delivered_to_b[1]) == payload,
              "the late retransmit delivered the wrong payload");
}

/*
 * Sweeping must not keep a dead receive alive.
 *
 * The stall deadline counts *arrivals*, not the messages this end emits — and
 * a sweep emits messages. Counting those would let a receive whose far helper
 * has gone reset its own deadline for ever, turning the fix for #145 into a
 * transfer that is never reported at all.
 */
void test_a_swept_receive_is_still_abandoned() {
    Pair pair;
    pair.copy_on_a("warm the seal");

    /* The far end never hears a request, so it never sends a chunk. */
    pair.drop_next[DH_MSG_CLIP_REQUEST] = 10000;
    pair.settle(pair.a.local_copy(ClipKind::Text, bytes_of("into the void")), Side::A);
    CHECK(pair.delivered_to_b.size() == 1, "the second copy was expected to stall");

    std::string abandoned;
    for (uint32_t now = 0; now <= ClipService::kStallTimeoutMs + 1;
         now += ClipService::kSweepDelayMs) {
        for (const ClipOutput &output : pair.b.tick(now))
            if (output.kind == ClipOutput::Kind::Note &&
                output.note.find("was abandoned") != std::string::npos)
                abandoned = output.note;
    }
    CHECK(!abandoned.empty(),
          "a receive that swept for the whole timeout was never abandoned");
    /* And the abandonment says what it asked for and what came back, which is
       the reading neither end produced before. */
    CHECK(abandoned.find("asked for") != std::string::npos &&
              abandoned.find("back") != std::string::npos,
          "the abandonment did not say whether a retransmit was asked for");
}

/*
 * A newer offer supersedes an incomplete receive, and the transfer that
 * replaces it is entitled to the whole deadline — not to whatever is left of
 * the one it displaced.
 *
 * The reset event for this timer is "something arrived for the transfer being
 * timed", and a supersede changes *which* transfer is being timed without
 * changing how much of it has arrived: both counts are zero. So the count
 * alone cannot see it, and the transfer that arrives second is abandoned for
 * the sins of the first — reported as thirty seconds of silence when it is
 * seconds old.
 */
void test_a_superseded_receive_resets_the_deadline() {
    Pair pair;
    pair.copy_on_a("warm the seal");

    /* The far end never hears a request, so nothing ever arrives. */
    pair.drop_next[DH_MSG_CLIP_REQUEST] = 10000;
    pair.settle(pair.a.local_copy(ClipKind::Text, bytes_of("the first copy")), Side::A);
    pair.b.tick(1); /* arms the deadline on the first transfer */

    /* A second copy supersedes it, most of the way through that deadline. */
    pair.settle(pair.a.local_copy(ClipKind::Text, bytes_of("the second copy")), Side::A);

    for (const ClipOutput &output : pair.b.tick(ClipService::kStallTimeoutMs + 1))
        CHECK(!(output.kind == ClipOutput::Kind::Note &&
                output.note.find("was abandoned") != std::string::npos),
              "a transfer seconds old was abandoned on the deadline of the one it replaced");
}

void test_a_lost_offer_retries_until_a_retention_boundary() {
    Pair pair;
    pair.copy_on_a("warm the seal");
    pair.drop_next[DH_MSG_CLIP_OFFER] = 1;
    pair.settle(pair.a.local_copy(ClipKind::Text, bytes_of("recovered offer")), Side::A);
    CHECK(pair.delivered_to_b.size() == 1, "the deliberately lost offer arrived");
    pair.settle(pair.a.tick(0), Side::A);
    pair.settle(pair.a.tick(ClipService::kSweepDelayMs), Side::A);
    CHECK(pair.delivered_to_b.size() == 2, "the lost offer was not retried");
    CHECK(pair.saw_note("retry action(s) were produced"),
          "successful offer recovery was not diagnosed");
    size_t recovery_notes = 0;
    for (const std::string &note : pair.notes)
        if (note.find("retry action(s) were produced") != std::string::npos) recovery_notes++;
    CHECK(recovery_notes == 1, "offer recovery was diagnosed more than once");
    bool retried_after_request = false;
    for (const ClipOutput &output : pair.a.tick(2 * ClipService::kSweepDelayMs))
        if (output.kind == ClipOutput::Kind::Send && output.type == DH_MSG_CLIP_OFFER)
            retried_after_request = true;
    CHECK(!retried_after_request, "offer retry continued after the request");

    Pair dead;
    dead.copy_on_a("warm the seal");
    dead.drop_next[DH_MSG_CLIP_OFFER] = 10000;
    dead.settle(dead.a.local_copy(ClipKind::Text, bytes_of("unanswered")), Side::A);
    for (uint32_t now = 0; now <= ClipService::kStallTimeoutMs; now += ClipService::kSweepDelayMs)
        dead.settle(dead.a.tick(now), Side::A);
    CHECK(!dead.saw_note("was abandoned"),
          "an unanswered offer was falsely diagnosed or abandoned");

    Pair duplicate;
    duplicate.copy_on_a("warm the seal");
    duplicate.drop_next[DH_MSG_CLIP_REQUEST] = 10000;
    duplicate.settle(duplicate.a.local_copy(ClipKind::Text, bytes_of("duplicate")), Side::A);
    for (uint32_t now = 0; now <= ClipService::kStallTimeoutMs;
         now += ClipService::kSweepDelayMs) {
        duplicate.settle(duplicate.a.tick(now), Side::A);
        duplicate.settle(duplicate.b.tick(now), Side::B);
    }
    CHECK(duplicate.saw_note("was abandoned") && duplicate.saw_note("duplicate offers"),
          "duplicate arrivals moved or hid the receive deadline");
}

void test_a_conflicting_authenticated_offer_ends_the_local_session() {
    ClipService service(seal_aead(), counter_entropy(1));
    dh_seal_tx sender{};
    dh_seal_tx_init(&sender);
    uint8_t private_key[DH_P256_PRIVATE_SIZE], nonce[DH_NONCE_SIZE];
    for (size_t i = 0; i < sizeof private_key; i++) private_key[i] = static_cast<uint8_t>(i + 3);
    for (size_t i = 0; i < sizeof nonce; i++) nonce[i] = static_cast<uint8_t>(i + 33);
    uint8_t exchange[DH_SEAL_EXCHANGE_LEN];
    size_t exchange_len = 0;
    CHECK(dh_seal_tx_offer(&sender, 77, private_key, nonce, exchange, sizeof exchange,
                           &exchange_len) == DH_SEAL_OK,
          "the test seal could not be offered");
    std::vector<ClipOutput> accepted =
        service.received(DH_MSG_SEAL_OFFER, exchange, exchange_len);
    const ClipOutput *reply = nullptr;
    for (const ClipOutput &output : accepted)
        if (output.kind == ClipOutput::Kind::Send && output.type == DH_MSG_SEAL_ACCEPT)
            reply = &output;
    CHECK(reply != nullptr, "the service did not accept the test seal");
    if (reply == nullptr) return;
    CHECK(dh_seal_tx_accepted(&sender, reply->bytes.data(), reply->bytes.size()) == DH_SEAL_OK,
          "the test seal accept was refused");

    uint8_t body[DH_FRAME_MAX_PAYLOAD];
    size_t body_len = 0;
    dh_clip_offer offer{9, 0, 1, nullptr, 0};
    CHECK(dh_seal_encode_offer(&sender, seal_aead(), &offer, body, sizeof body, &body_len) ==
              DH_SEAL_OK,
          "the first identity could not be sealed");
    (void)service.received(DH_MSG_CLIP_OFFER, body, body_len);
    offer.total = 2;
    CHECK(dh_seal_encode_offer(&sender, seal_aead(), &offer, body, sizeof body, &body_len) ==
              DH_SEAL_OK,
          "the conflicting identity could not be sealed");
    const std::vector<ClipOutput> outputs = service.received(DH_MSG_CLIP_OFFER, body, body_len);
    bool protocol_error = false;
    for (const ClipOutput &output : outputs)
        if (output.kind == ClipOutput::Kind::ProtocolError) protocol_error = true;
    CHECK(protocol_error, "an authenticated offer identity conflict did not end the local session");
}

/*
 * The asymmetric restart (#151): the far computer's helper process goes away
 * and comes back while this end's session never falters.
 *
 * Offer ids are ordered inside the *copy side helper's* namespace, so a fresh
 * process starts again at one. Without a boundary at the fresh seal, this end
 * measures that one against the dead process's offer-id frontier, calls it stale, and
 * the clipboard stays dead in that direction until this end resets too.
 */
void test_a_restarted_far_helper_is_heard() {
    Pair pair;
    pair.copy_on_a("first");
    pair.copy_on_a("second"); /* the frontier is now above one */
    CHECK(pair.delivered_to_b.size() == 2, "the copies before the restart did not arrive");

    pair.restart_a();
    pair.copy_on_a("after the restart");

    CHECK(pair.delivered_to_b.size() == 3,
          "the restarted helper's first offer was ignored as stale");
    if (pair.delivered_to_b.size() == 3)
        CHECK(text_of(pair.delivered_to_b[2]) == "after the restart",
              "the restarted helper's offer carried the wrong payload");
}

/*
 * The same restart one copy earlier, where the reused id is not older but
 * *equal* — and the payload behind it is a different one. Answered as a fresh
 * transfer, not as an identity conflict, which would end the session over a far
 * helper doing nothing wrong.
 */
void test_a_restarted_id_is_not_a_conflict() {
    Pair pair;
    pair.copy_on_a("first");
    CHECK(pair.delivered_to_b.size() == 1, "the copy before the restart did not arrive");

    pair.restart_a();
    pair.copy_on_a("a different payload under the same id");

    CHECK(pair.delivered_to_b.size() == 2,
          "the restarted helper's reused id did not carry its payload");
    if (pair.delivered_to_b.size() == 2)
        CHECK(text_of(pair.delivered_to_b[1]) == "a different payload under the same id",
              "the reused id carried the wrong payload");
    CHECK(!pair.saw_note("protocol error"),
          "a restarted helper's reused offer id was read as a conflict");
}

/*
 * A half-arrived transfer belongs to the seal it arrived under. The helper that
 * sent it has forgotten it, so it can never be finished — abandoned whole, and
 * never written to the clipboard in part.
 */
void test_a_replaced_seal_abandons_the_receive() {
    Pair pair;
    pair.copy_on_a("first, to establish a seal");

    /* Every chunk of the second copy is refused at a seam with no retransmit
       beneath it, so B is left holding an offer and no payload. */
    pair.drop_next[DH_MSG_CLIP_CHUNK] = 10000;
    pair.copy_on_a(std::string(5000, 'x'));
    CHECK(pair.delivered_to_b.size() == 1, "the payload arrived through a link that dropped it");

    pair.drop_next.clear();
    pair.restart_a();
    pair.copy_on_a("after the restart");

    CHECK(pair.delivered_to_b.size() == 2,
          "a partial payload was delivered, or the copy after the restart was not");
    if (pair.delivered_to_b.size() == 2)
        CHECK(text_of(pair.delivered_to_b[1]) == "after the restart",
              "what arrived after the restart was not the payload that was copied");
    CHECK(pair.saw_note("abandoned: the far helper started a fresh seal"),
          "the receive under the replaced seal was abandoned without saying why");
}

/*
 * The straggler. An offer sealed under the replaced key can still be in flight
 * when the fresh one is accepted, and arriving late it must not be able to
 * recreate the receive state that was just given up. It cannot be opened at
 * all: this end holds one incoming seal, and the fresh one replaced it.
 */
void test_a_delayed_offer_cannot_revive() {
    Pair pair;
    pair.copy_on_a("first, to establish a seal");
    std::vector<uint8_t> old_offer;
    for (const auto &frame : pair.carried_frames)
        if (frame.first == DH_MSG_CLIP_OFFER) old_offer = frame.second;
    CHECK(!old_offer.empty(), "no offer was carried to hold back");
    if (old_offer.empty()) return;

    pair.restart_a();
    pair.copy_on_a("after the restart");

    const std::vector<ClipOutput> outputs =
        pair.b.received(DH_MSG_CLIP_OFFER, old_offer.data(), old_offer.size());
    bool said_stale = false;
    for (const ClipOutput &output : outputs)
        if (output.kind == ClipOutput::Kind::Send && output.type == DH_MSG_SEAL_STALE)
            said_stale = true;
    CHECK(said_stale, "an offer under the replaced seal was opened rather than refused");
    CHECK(pair.delivered_to_b.size() == 2,
          "a delayed offer under the replaced seal changed what arrived");
}

} // namespace


/* -------------------------------------------------------------- files (#56)
 *
 * The twin of ClipboardTests.swift's file section, checking the same claims.
 * The two suites existing separately is the point: a divergence between them
 * is a clipboard that works on one computer and not the other.
 */

/* The file list these copy, and the payload it names. Written out rather than
   generated, so a change to how sizes and offsets are handled has a fixed set
   of numbers to disagree with. */
std::vector<deskhop::FileEntry> three_files() {
    return {deskhop::FileEntry{"notes.txt", 5}, deskhop::FileEntry{"empty.bin", 0},
            deskhop::FileEntry{"data.png", 11}};
}
std::vector<uint8_t> three_file_payload() { return bytes_of("helloworld other"); }

/* A set over kFilePromptThreshold, which is the only kind put to the user.
   The set above deliberately is not: below the threshold a transfer is a
   fraction of a second, and asking about it is how the prompt that matters
   gets dismissed unread. */
std::vector<deskhop::FileEntry> big_files() {
    return {deskhop::FileEntry{"big.bin", 300u * 1024u}};
}
std::vector<uint8_t> big_payload() { return std::vector<uint8_t>(300u * 1024u, 0x5a); }

/*
 * A copy made while the link is reconnecting is held, not thrown away.
 *
 * On a thrashing link a copy lands with no seal to send it under, so it parks.
 * The fault this pins was that the *next* session end dropped the parked copy
 * without a word — at the desk the file was copied, no question was ever put to
 * the far side, and neither helper's log said anything at all. Silence is the
 * part that made it unfindable, so both halves are checked: the copy still goes
 * out, and the wait is on the record.
 */
void test_a_copy_waiting_for_a_seal_survives_a_session_end() {
    Pair pair(1024u * 1024u);

    /* The seal offer is lost, so the copy has no key and has to wait. */
    pair.drop_next[DH_MSG_SEAL_OFFER] = 1;
    pair.copy_files_on_a(big_files(), big_payload());
    CHECK(pair.file_questions.empty(),
          "the files were offered with no seal to send them under");
    CHECK(pair.saw_note("waiting for a seal"), "a copy parked with nothing said about it");

    /* The link wobbles again before the seal lands. */
    pair.settle(pair.a.session_ended(), Side::A);

    /* And now it comes back. The copy that was waiting goes out on its own,
       without the user having to copy it a second time. */
    (void)pair.a.tick(0);
    pair.settle(pair.a.tick(ClipService::kSweepDelayMs), Side::A);
    CHECK(pair.file_questions.size() == 1,
          "a copy made while the link was down never went out after it came back");
    CHECK(pair.files_to_b.size() == 1 && pair.files_to_b[0].bytes == big_payload(),
          "the copy that survived the wobble did not arrive whole");
}


void test_files_cross_the_link() {
    Pair pair;
    pair.copy_files_on_a(three_files(), three_file_payload());

    CHECK(pair.files_to_b.size() == 1, "the files copied on A did not arrive on B");
    if (pair.files_to_b.empty()) return;
    CHECK(pair.files_to_b[0].files == three_files(), "the file list did not survive the link");
    CHECK(pair.files_to_b[0].bytes == three_file_payload(),
          "the file payload was not byte-identical end to end");
    CHECK(pair.files_to_a.empty(), "A was handed its own files back");
}

/* The whole of #56, in one check: a copy costs nothing until someone on the
   other computer says yes. */
void test_files_are_not_read_until_accepted() {
    Pair pair(1024u * 1024u);
    pair.answer_file_offers = false;
    int reads = 0;

    pair.copy_files_on_a(big_files(), big_payload(), &reads);
    CHECK(reads == 0, "the copied files were read before anyone accepted them");
    CHECK(pair.file_questions.size() == 1, "B was not asked about the files");
    if (pair.file_questions.empty()) return;
    CHECK(pair.file_questions[0].files == big_files(),
          "the question did not name the files that were offered");
    CHECK(pair.files_to_b.empty(), "files were delivered without being accepted");
    CHECK(pair.b.awaiting_decision() != nullptr, "the offer is not being held for an answer");

    pair.settle(pair.b.accept_files(pair.file_questions[0].id), Side::B);
    CHECK(reads == 1, "accepting the files did not read them exactly once");
    CHECK(!pair.files_to_b.empty() && pair.files_to_b[0].bytes == big_payload(),
          "the accepted files did not arrive");
    CHECK(pair.b.awaiting_decision() == nullptr,
          "the offer is still being held after an answer");
}

void test_declining_files_reads_nothing() {
    Pair pair(1024u * 1024u);
    pair.accept_file_offers = false;
    int reads = 0;

    pair.copy_files_on_a(big_files(), big_payload(), &reads);
    CHECK(reads == 0, "declined files were read anyway");
    CHECK(pair.files_to_b.empty(), "declined files were delivered");
    CHECK(!pair.withdrawn_questions.empty(), "declining did not take the question back");
    CHECK(!pair.a.awaiting_send(), "the declined transfer is still being offered");
}

void test_small_file_sets_skip_the_question() {
    Pair pair;
    pair.answer_file_offers = false;
    pair.copy_files_on_a({deskhop::FileEntry{"tiny.txt", 4}}, bytes_of("abcd"));

    CHECK(pair.file_questions.empty(),
          "a set well under the threshold asked a question anyway");
    CHECK(!pair.files_to_b.empty() && pair.files_to_b[0].bytes == bytes_of("abcd"),
          "a set under the threshold did not arrive on its own");
}

void test_several_files_split_correctly() {
    Pair pair(1024u * 1024u);
    const std::vector<deskhop::FileEntry> files = {deskhop::FileEntry{"a", 1000},
                                                   deskhop::FileEntry{"b", 1},
                                                   deskhop::FileEntry{"c", 2000}};
    std::vector<uint8_t> payload(1000, 0x11);
    payload.push_back(0x22);
    payload.insert(payload.end(), 2000, 0x33);
    pair.copy_files_on_a(files, payload);

    CHECK(pair.files_to_b.size() == 1, "the three files did not arrive");
    if (pair.files_to_b.empty()) return;
    CHECK(pair.files_to_b[0].files == files, "the sizes did not survive");
    const uint8_t expected[3] = {0x11, 0x22, 0x33};
    size_t at = 0;
    for (size_t i = 0; i < pair.files_to_b[0].files.size(); i++) {
        const size_t size = static_cast<size_t>(pair.files_to_b[0].files[i].size);
        bool all = true;
        for (size_t j = 0; j < size; j++)
            if (pair.files_to_b[0].bytes[at + j] != expected[i]) all = false;
        CHECK(all, "a file did not slice out of the payload at the right offset");
        at += size;
    }
}

/*
 * The offer's total and its list come from the same far helper, so a
 * disagreement between them is that helper being wrong or being tampered with.
 *
 * Checked at the codec's own seam. The sum is the core's — it adds the sizes
 * once, with an overflow check, and hands the total back — so what is asserted
 * here is that the total which comes back is the one the service compares.
 */
void test_a_mismatched_file_list_is_refused() {
    std::vector<dh_file_entry> raw;
    const std::vector<deskhop::FileEntry> files = three_files();
    for (const deskhop::FileEntry &file : files)
        raw.push_back(dh_file_entry{file.name.data(),
                                    static_cast<uint16_t>(file.name.size()), file.size});
    std::vector<char> buffer(dh_file_list_encode_max());
    const int written = dh_file_list_encode(raw.data(), static_cast<uint16_t>(raw.size()),
                                            buffer.data(), buffer.size());
    CHECK(written > 0, "the three-file list would not encode");

    dh_file_list list{};
    CHECK(dh_file_list_decode(buffer.data(), static_cast<size_t>(written), &list),
          "the encoded list would not decode");
    CHECK(list.total == 16, "the core's total is not the sum of the list");
    CHECK(list.count == files.size(), "the list did not survive the round trip");

    /* And sizes that overflow their total are refused outright, so no total
       ever comes back for the service to compare. */
    static const char *const overflowing =
        "[{\"name\":\"a\",\"size\":18446744073709551615},{\"name\":\"b\",\"size\":1}]";
    CHECK(!dh_file_list_decode(overflowing, std::strlen(overflowing), &list),
          "sizes that overflow their total were accepted");
}

/*
 * A file edited between the copy and the paste no longer reads at the length
 * that was offered. The core is about to read exactly the offered length from
 * whatever it is handed, so a short read here would be an overread there.
 */
void test_a_short_read_fails_rather_than_truncates() {
    for (size_t wrong : {size_t{8}, size_t{32}}) {
        Pair pair;
        const std::vector<uint8_t> payload(wrong, 1);
        pair.settle(pair.a.local_copy_files(three_files(),
                                            [payload](std::vector<uint8_t> &out) {
                                                out = payload;
                                                return true;
                                            }),
                    Side::A);
        CHECK(pair.files_to_b.empty(), "a payload of the wrong length was delivered");
        CHECK(pair.saw_note("abandoned rather than sent short"),
              "a payload that did not match its offer failed silently");
        CHECK(!pair.a.awaiting_send(), "the mismatched transfer is still being offered");
    }
}

void test_unreadable_files_fail_the_transfer() {
    Pair pair;
    pair.settle(pair.a.local_copy_files(three_files(),
                                        [](std::vector<uint8_t> &) { return false; }),
                Side::A);
    CHECK(pair.files_to_b.empty(), "files that could not be read were delivered anyway");
    CHECK(pair.saw_note("could not be read"), "files that could not be read failed silently");
    CHECK(!pair.a.awaiting_send(), "the failed transfer is still being offered");
}

void test_files_over_the_cap_are_refused() {
    Pair pair(64u * 1024u);
    pair.answer_file_offers = false;
    int reads = 0;
    pair.copy_files_on_a({deskhop::FileEntry{"big.bin", 128u * 1024u}},
                         std::vector<uint8_t>(128u * 1024u, 0), &reads);

    CHECK(pair.file_questions.empty(),
          "a set over the size cap was put to the user rather than refused");
    CHECK(reads == 0, "a set over the size cap was read anyway");
    CHECK(pair.files_to_b.empty(), "a set over the size cap was delivered");
}

void test_the_boards_size_cap_is_applied() {
    Pair pair(1024u * 1024u);
    pair.answer_file_offers = false;

    /* The board says one megabyte. What was already in force is irrelevant —
       the device is the single source of truth (#42). */
    pair.settle(pair.b.capacity_changed(1), Side::B);

    int reads = 0;
    pair.copy_files_on_a({deskhop::FileEntry{"big.bin", 2u * 1024u * 1024u}},
                         std::vector<uint8_t>(2u * 1024u * 1024u, 0), &reads);
    CHECK(pair.files_to_b.empty(), "a set over the board's cap was delivered");
    CHECK(reads == 0, "a set over the board's cap was read");

    pair.copy_files_on_a(three_files(), three_file_payload());
    CHECK(pair.files_to_b.size() == 1, "a set inside the board's cap did not arrive");
}

void test_a_held_question_is_withdrawn() {
    Pair pair(1024u * 1024u);
    pair.answer_file_offers = false;
    pair.copy_files_on_a(big_files(), big_payload());
    CHECK(!pair.file_questions.empty(), "no question was asked");
    if (pair.file_questions.empty()) return;
    const uint32_t id = pair.file_questions[0].id;

    pair.settle(pair.b.session_ended(), Side::B);
    bool withdrawn = false;
    for (uint32_t seen : pair.withdrawn_questions)
        if (seen == id) withdrawn = true;
    CHECK(withdrawn,
          "a session that ended left a question standing over a transfer that is gone");
    CHECK(pair.b.awaiting_decision() == nullptr,
          "the offer is still held after the session ended");
}

/*
 * The two directions are independent, and a failure in one must not take the
 * other's state with it. Reachable in the ordinary way: this computer offers
 * files whose bytes can no longer be read while it is still holding a question
 * about files the other computer offered. Transfer ids collide across the two
 * directions (#136), so the id on the failure cannot say which one it was.
 */
void test_a_failed_send_leaves_a_healthy_receive_alone() {
    Pair pair(1024u * 1024u);
    pair.answer_file_offers = false;
    pair.copy_files_on_a(big_files(), big_payload());
    CHECK(!pair.file_questions.empty(), "no question was asked");
    if (pair.file_questions.empty()) return;
    const uint32_t id = pair.file_questions[0].id;

    /* B now tries to send files of its own, and cannot read them. */
    pair.settle(pair.b.local_copy_files(three_files(),
                                        [](std::vector<uint8_t> &) { return false; }),
                Side::B);
    CHECK(pair.saw_note("could not be read"), "B's send did not fail");

    CHECK(pair.b.awaiting_decision() != nullptr &&
              pair.b.awaiting_decision()->id == id,
          "a failed send withdrew the question B was still holding");
    CHECK(pair.withdrawn_questions.empty(),
          "a failed send took back a question about the other direction");

    /*
     * How far this can be taken today. Accepting does *not* deliver, and for a
     * reason outside this file: B's cancel names its own transfer id, A's
     * outgoing transfer holds the same id, and A abandons it (#136 —
     * "transfer ids collide across directions"). That is a live bug with a
     * ticket of its own, and #56 gives it a new way to happen, since a file
     * send that cannot read its files is a fresh source of CLIP_CANCEL.
     *
     * What is asserted above is the part this file owns: the state is kept, so
     * the acceptance still reaches a transfer rather than falling on the floor.
     * When #136 lands, this becomes a delivery.
     */
    pair.settle(pair.b.accept_files(id), Side::B);
    CHECK(pair.saw_note("were accepted here and asked for"),
          "the acceptance did not reach a transfer at all");
}

/*
 * A text copy made while a file question is still standing supersedes it inside
 * the transfer machine. The question has to go with it: left up, the tray goes
 * on offering Accept for a transfer the far end has moved past, where accepting
 * does nothing and says nothing either.
 */
void test_a_newer_copy_withdraws_a_held_question() {
    Pair pair(1024u * 1024u);
    pair.answer_file_offers = false;
    pair.copy_files_on_a(big_files(), big_payload());
    CHECK(!pair.file_questions.empty(), "no question was asked");
    if (pair.file_questions.empty()) return;
    const uint32_t id = pair.file_questions[0].id;

    pair.copy_on_a("something else entirely");
    bool withdrawn = false;
    for (uint32_t seen : pair.withdrawn_questions)
        if (seen == id) withdrawn = true;
    CHECK(withdrawn, "a newer copy left the question about the old transfer standing");
    CHECK(pair.b.awaiting_decision() == nullptr, "the superseded offer is still held");
    CHECK(!pair.delivered_to_b.empty() &&
              text_of(pair.delivered_to_b.back()) == "something else entirely",
          "the newer copy did not arrive");
}

/*
 * A question nobody answers cannot stand for ever. The copy side re-offers
 * every two seconds until its offer is requested, so an ignored prompt is a
 * frame every two seconds for the life of the session — and the receive buffer
 * the size cap sizes stays pinned while it stands.
 */
void test_an_unanswered_question_is_declined_in_the_end() {
    Pair pair(1024u * 1024u);
    pair.answer_file_offers = false;
    pair.copy_files_on_a(big_files(), big_payload());
    CHECK(!pair.file_questions.empty(), "no question was asked");
    if (pair.file_questions.empty()) return;
    const uint32_t id = pair.file_questions[0].id;

    /* The first tick arms the deadline; nothing expires on it. */
    pair.settle(pair.b.tick(1000), Side::B);
    CHECK(pair.b.awaiting_decision() != nullptr, "the first tick declined it outright");

    pair.settle(pair.b.tick(1000 + ClipService::kHoldTimeoutMs - 1), Side::B);
    CHECK(pair.b.awaiting_decision() != nullptr, "it was declined a millisecond early");

    pair.settle(pair.b.tick(1000 + ClipService::kHoldTimeoutMs), Side::B);
    CHECK(pair.b.awaiting_decision() == nullptr,
          "an unanswered question stood past its deadline");
    bool withdrawn = false;
    for (uint32_t seen : pair.withdrawn_questions)
        if (seen == id) withdrawn = true;
    CHECK(withdrawn, "the expired question was not taken back");
    CHECK(pair.files_to_b.empty(), "an expired question delivered its files anyway");
    CHECK(!pair.a.awaiting_send(), "the copy side is still offering a declined transfer");
}

/*
 * A set past the size cap is *refused*, not put to the user.
 *
 * Hardware, 2026-09-02: with the cap at 2 MB a 2.46 MB file was toasted, Accept
 * did nothing, and the file never arrived. The predicate asked whether an offer
 * had been *seen*, which stays true for one the machine has already cancelled.
 */
void test_an_over_cap_set_is_never_put_to_the_user() {
    Pair pair(1024u * 1024u * 4u);
    pair.answer_file_offers = false;
    pair.settle(pair.b.capacity_changed(2), Side::B);

    int reads = 0;
    pair.copy_files_on_a({deskhop::FileEntry{"big.bin", 2581661}},
                         std::vector<uint8_t>(2581661, 0), &reads);

    CHECK(pair.file_questions.empty(),
          "a set past the size cap was put to the user rather than refused");
    CHECK(reads == 0, "a set past the size cap was read");
    CHECK(pair.files_to_b.empty(), "a set past the size cap was delivered");
    CHECK(pair.b.awaiting_decision() == nullptr, "a refused offer is being held for an answer");
}

/*
 * The copy side repeats its offer every two seconds until it is requested
 * (#78). Once the answer has gone out, those repeats must not ask again.
 *
 * Hardware, 2026-09-02: one file produced three toasts and three Accepts.
 */
void test_offer_retries_do_not_re_ask_after_the_answer() {
    Pair pair(1024u * 1024u);
    pair.answer_file_offers = false;
    pair.copy_files_on_a(big_files(), big_payload());
    CHECK(!pair.file_questions.empty(), "no question was asked");
    if (pair.file_questions.empty()) return;

    std::pair<uint8_t, std::vector<uint8_t>> sent;
    bool have_sent = false;
    for (const auto &frame : pair.carried_frames)
        if (frame.first == DH_MSG_CLIP_OFFER) { sent = frame; have_sent = true; }
    CHECK(have_sent, "no offer crossed the link");
    if (!have_sent) return;

    pair.settle(pair.b.accept_files(pair.file_questions[0].id), Side::B);
    CHECK(pair.file_questions.size() == 1, "accepting re-asked the question");

    /* The copy side's retry, arriving after the answer — the frame that
       crossed with the request on hardware. */
    for (int i = 0; i < 3; i++)
        pair.settle(pair.b.received(sent.first, sent.second.data(), sent.second.size()), Side::B);

    CHECK(pair.file_questions.size() == 1,
          "an offer retry re-asked a question the user had already answered");
    CHECK(pair.files_to_b.size() == 1, "the accepted transfer did not arrive");
}

/*
 * Accepting an offer the machine is no longer holding must not remember its
 * file list. It did, and the *next* transfer was then split by the wrong list
 * and silently written nowhere.
 */
void test_an_accept_that_cannot_run_poisons_nothing() {
    Pair pair(1024u * 1024u * 4u);
    pair.answer_file_offers = false;
    pair.settle(pair.b.capacity_changed(2), Side::B);

    pair.copy_files_on_a({deskhop::FileEntry{"big.bin", 2581661}},
                         std::vector<uint8_t>(2581661, 0));
    pair.settle(pair.b.accept_files(1), Side::B);

    pair.answer_file_offers = true;
    pair.copy_files_on_a(three_files(), three_file_payload());
    CHECK(pair.files_to_b.size() == 1, "a healthy transfer did not arrive after a dead accept");
    CHECK(!pair.files_to_b.empty() && pair.files_to_b[0].bytes == three_file_payload(),
          "a healthy transfer was split by a dead transfer's list");
}

void test_an_accepted_transfer_reports_progress() {
    Pair pair(1024u * 1024u);
    pair.answer_file_offers = false;
    pair.copy_files_on_a(big_files(), big_payload());

    CHECK(!pair.b.arriving(nullptr, nullptr, nullptr),
          "a held offer reported itself as arriving");
    CHECK(!pair.file_questions.empty(), "no question was asked about a 300 KB set");
    if (pair.file_questions.empty()) return;
    CHECK(pair.file_questions[0].total == 300u * 1024u, "the question named the wrong size");
    CHECK(pair.file_questions[0].estimated_seconds() >= 6,
          "the estimate for 300 KB at the measured rate is implausibly short");

    pair.settle(pair.b.accept_files(pair.file_questions[0].id), Side::B);
    CHECK(pair.files_to_b.size() == 1, "the accepted set did not arrive");
    CHECK(!pair.b.arriving(nullptr, nullptr, nullptr),
          "a finished transfer still reports itself as arriving");
}

void test_a_transfer_can_be_aborted_here() {
    Pair pair(1024u * 1024u);
    pair.answer_file_offers = false;
    pair.copy_files_on_a(big_files(), big_payload());
    CHECK(!pair.file_questions.empty(), "no question was asked");

    pair.settle(pair.b.abort_receive(), Side::B);
    CHECK(pair.files_to_b.empty(), "an aborted transfer was delivered");
    CHECK(pair.b.awaiting_decision() == nullptr, "the aborted offer is still held");
}

/* Colliding names are renamed, never overwritten — the rule both helpers
   follow, so a delivery comes back under the names it went out as. */
void test_colliding_names_are_renamed() {
    std::set<std::string> used;
    CHECK(deskhop::unused_file_name("report.pdf", used) == "report.pdf", "the first was renamed");
    CHECK(deskhop::unused_file_name("report.pdf", used) == "report-2.pdf",
          "the second did not get a suffix before the extension");
    CHECK(deskhop::unused_file_name("report.pdf", used) == "report-3.pdf",
          "the third collided with the second");
    CHECK(deskhop::unused_file_name("noext", used) == "noext", "a name with no extension");
    CHECK(deskhop::unused_file_name("noext", used) == "noext-2",
          "a name with no extension got a stray dot");

    std::set<std::string> hidden{".gitignore"};
    CHECK(deskhop::unused_file_name(".gitignore", hidden) == ".gitignore-2",
          "a leading dot was treated as an extension");
}

int main() {
    if (seal_aead() == nullptr) {
        std::printf("FAIL this machine has no AES-GCM provider; nothing here can run\n");
        return 1;
    }

    test_text_crosses_the_link();
    test_image_crosses_the_link();
    test_a_lazy_offer_retry_does_not_reclaim_the_clipboard();
    test_replacing_a_lazy_image_cancels_its_receive();
    test_fidelity_is_preserved();
    test_nothing_leaves_unsealed();
    test_a_lost_seal_offer_is_retried();
    test_a_lost_seal_accept_is_retried();
    test_seal_retries_do_not_extend_the_copy_deadline();
    test_a_multi_chunk_payload_arrives();
    test_a_second_copy_supersedes();
    test_sending_off_stops_one_direction();
    test_receiving_off_refuses_the_offer();
    test_a_turned_off_toggle_abandons_in_flight();
    test_a_copy_waiting_for_a_seal_survives_a_session_end();
    test_a_lost_session_abandons_everything();
    test_a_stale_seal_is_reoffered();
    test_malformed_control_messages();
    test_a_stalled_send_is_retained();
    test_a_stall_says_what_the_board_has_dropped();
    test_progress_keeps_a_transfer_alive();
    test_chunks_are_refused_before_the_seal_is_opened();
    test_a_stalled_receive_asks_again();
    test_a_copy_side_deadline_does_not_defeat_a_later_receive_sweep();
    test_a_swept_receive_is_still_abandoned();
    test_a_superseded_receive_resets_the_deadline();
    test_a_lost_offer_retries_until_a_retention_boundary();
    test_a_conflicting_authenticated_offer_ends_the_local_session();
    test_a_restarted_far_helper_is_heard();
    test_a_restarted_id_is_not_a_conflict();
    test_a_replaced_seal_abandons_the_receive();
    test_a_delayed_offer_cannot_revive();

    test_files_cross_the_link();
    test_files_are_not_read_until_accepted();
    test_declining_files_reads_nothing();
    test_small_file_sets_skip_the_question();
    test_several_files_split_correctly();
    test_a_mismatched_file_list_is_refused();
    test_a_short_read_fails_rather_than_truncates();
    test_unreadable_files_fail_the_transfer();
    test_files_over_the_cap_are_refused();
    test_the_boards_size_cap_is_applied();
    test_a_held_question_is_withdrawn();
    test_an_accepted_transfer_reports_progress();
    test_a_transfer_can_be_aborted_here();
    test_a_failed_send_leaves_a_healthy_receive_alone();
    test_a_newer_copy_withdraws_a_held_question();
    test_an_unanswered_question_is_declined_in_the_end();
    test_an_over_cap_set_is_never_put_to_the_user();
    test_offer_retries_do_not_re_ask_after_the_answer();
    test_an_accept_that_cannot_run_poisons_nothing();
    test_colliding_names_are_renamed();

    if (failures > 0) {
        std::printf("%d clipboard check(s) failed\n", failures);
        return 1;
    }
    std::printf("clipboard tests passed\n");
    return 0;
}
