/*
 * The clipboard text path (#52), driven as **two helpers talking to each
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
#include <string>
#include <utility>
#include <vector>

#include "clip_service.h"
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
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::string text_of(const std::vector<uint8_t> &bytes) {
    return std::string(bytes.begin(), bytes.end());
}

/*
 * Entropy that is deterministic but not constant: a seal needs a fresh
 * ephemeral key and nonce per exchange, and a source that returned the same
 * bytes twice would key both directions identically and hide a real mix-up.
 */
std::function<void(uint8_t *, size_t)> counter_entropy(uint8_t seed) {
    auto step = std::make_shared<uint8_t>(0);
    return [seed, step](uint8_t *out, size_t len) {
        ++*step;
        for (size_t i = 0; i < len; i++)
            out[i] = static_cast<uint8_t>(i * 31u + seed * 7u + *step);
    };
}

enum class Side { A, B };

/* Two helpers and the link between them. */
struct Pair {
    ClipService a{seal_aead(), counter_entropy(1), 64u * 1024u};
    ClipService b{seal_aead(), counter_entropy(2), 64u * 1024u};
    std::vector<std::vector<uint8_t>> delivered_to_a;
    std::vector<std::vector<uint8_t>> delivered_to_b;
    std::vector<std::string> notes;
    /* Frames carried across the link, so a test can say what a direction cost. */
    size_t carried = 0;

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
                ClipService &far = side == Side::A ? b : a;
                const Side far_side = side == Side::A ? Side::B : Side::A;
                for (ClipOutput &item :
                     far.received(output.type, output.bytes.data(), output.bytes.size()))
                    queue.emplace_back(far_side, std::move(item));
                /* A frame going out is a chance to push the next credit-gated
                   batch, which is what the run loop does on the same seam. */
                ClipService &near = side == Side::A ? a : b;
                for (ClipOutput &item : near.pump()) queue.emplace_back(side, std::move(item));
                break;
            }
            case ClipOutput::Kind::Deliver:
                (side == Side::A ? delivered_to_a : delivered_to_b).push_back(output.bytes);
                break;
            case ClipOutput::Kind::Note:
                notes.push_back(output.note);
                break;
            }
        }
    }

    void copy_on_a(const std::string &text) {
        settle(a.local_copy(ClipKind::Text, bytes_of(text)), Side::A);
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

    CHECK(first.size() == 1, "a copy with no seal produced more than the seal offer");
    if (first.empty()) return;
    CHECK(first[0].kind == ClipOutput::Kind::Send, "a copy with no seal sent nothing");
    CHECK(first[0].type == DH_MSG_SEAL_OFFER, "the first thing sent was not a seal offer");
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
    pair.settle(pair.a.policy_changed(DH_CLIP_MAY_RECEIVE), Side::A);
    pair.settle(pair.b.policy_changed(DH_CLIP_MAY_SEND), Side::B);

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
    pair.settle(pair.b.policy_changed(DH_CLIP_MAY_SEND), Side::B);

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
    for (ClipOutput &item : pair.a.policy_changed(DH_CLIP_MAY_RECEIVE))
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
    const std::vector<ClipOutput> unknown = a.received(0x3F, nullptr, 0);
    CHECK(unknown.size() == 1 && unknown[0].kind == ClipOutput::Kind::Note,
          "an unknown clipboard message vanished without a word");
}

/*
 * The third interruption #52 names: the helper on the *other* computer crashes.
 * This end's session is untouched — it simply stops being answered — so nothing
 * message-driven can notice, and without the tick the transfer would sit
 * holding its payload until the next copy happened to supersede it.
 */
void test_a_stalled_transfer_is_abandoned() {
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

    /* Well inside the timeout, nothing happens: abandoning a healthy transfer
       is the worse of the two mistakes. */
    CHECK(a.tick(1).empty(), "a transfer was abandoned on its first tick");
    CHECK(a.tick(ClipService::kStallTimeoutMs - 1).empty(),
          "a transfer was abandoned before the timeout elapsed");

    bool reported = false;
    for (const ClipOutput &output : a.tick(ClipService::kStallTimeoutMs + 1))
        if (output.kind == ClipOutput::Kind::Note &&
            output.note.find("no progress") != std::string::npos)
            reported = true;
    CHECK(reported, "a stalled transfer was never abandoned, or never reported");

    /* And it is gone: a second timeout produces nothing to abandon. */
    CHECK(a.tick(ClipService::kStallTimeoutMs * 3).empty(),
          "the abandoned transfer was abandoned twice");
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

} // namespace

int main() {
    if (seal_aead() == nullptr) {
        std::printf("FAIL this machine has no AES-GCM provider; nothing here can run\n");
        return 1;
    }

    test_text_crosses_the_link();
    test_fidelity_is_preserved();
    test_nothing_leaves_unsealed();
    test_a_multi_chunk_payload_arrives();
    test_a_second_copy_supersedes();
    test_sending_off_stops_one_direction();
    test_receiving_off_refuses_the_offer();
    test_a_turned_off_toggle_abandons_in_flight();
    test_a_lost_session_abandons_everything();
    test_a_stale_seal_is_reoffered();
    test_malformed_control_messages();
    test_a_stalled_transfer_is_abandoned();
    test_progress_keeps_a_transfer_alive();

    if (failures > 0) {
        std::printf("%d clipboard check(s) failed\n", failures);
        return 1;
    }
    std::printf("clipboard tests passed\n");
    return 0;
}
