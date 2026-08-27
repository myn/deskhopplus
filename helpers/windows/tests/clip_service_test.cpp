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
#include <map>
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
    ClipService a{seal_aead(), counter_entropy(1), 64u * 1024u};
    ClipService b{seal_aead(), counter_entropy(2), 64u * 1024u};
    std::vector<std::vector<uint8_t>> delivered_to_a;
    std::vector<std::vector<uint8_t>> delivered_to_b;
    std::vector<std::string> notes;
    /* Frames carried across the link, so a test can say what a direction cost. */
    size_t carried = 0;
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
    void restart_a() { a = ClipService(seal_aead(), counter_entropy(3), 64u * 1024u); }

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

int main() {
    if (seal_aead() == nullptr) {
        std::printf("FAIL this machine has no AES-GCM provider; nothing here can run\n");
        return 1;
    }

    test_text_crosses_the_link();
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

    if (failures > 0) {
        std::printf("%d clipboard check(s) failed\n", failures);
        return 1;
    }
    std::printf("clipboard tests passed\n");
    return 0;
}
