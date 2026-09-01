/*
 * The shim's dispatch (#152): that each output kind reaches the effect it
 * names.
 *
 * This is the layer #93 and #94 are shaped like — a helper reporting healthy
 * while doing nothing. `HelperSession` and `ClipService` are both covered in
 * depth, and until this file existed the code that turned each of their
 * outputs into a real effect was correct by reading only.
 *
 * What is *not* checked here is Win32. The tray, the HID transport, this
 * computer's clipboard and the secret store stay behind `HelperEffects`, which
 * a `Recorder` implements by writing down what it was asked to do. That is the
 * whole reason the dispatch was split out of main.cpp, and it is why this test
 * runs on any machine rather than only on the one with the device attached.
 *
 * Style follows autostart_ladder_test.cpp — an assertion macro, a main, a
 * printed failure line, a non-zero exit (ADR-0006).
 */

#include <cstdio>
#include <string>
#include <vector>

#include "output_dispatch.h"

using namespace deskhop;

static int failures = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, (what));      \
        }                                                                    \
    } while (0)

/*
 * Every effect, written down in the order it was asked for.
 *
 * One list rather than a flag per effect, deliberately: the ordering is part
 * of what is being claimed — a refused frame must be counted *and* said out
 * loud, and a protocol error must be logged before the connection goes.
 */
class Recorder : public HelperEffects {
  public:
    std::vector<std::string> effects;

    /* What the platform answers back, for the arms that branch on it. */
    bool store_succeeds = true;
    bool transport_takes = true;
    bool frame_builds = true;
    std::vector<ClipOutput> policy_reply;

    bool store_board_key(const std::vector<uint8_t> &key) override {
        effects.push_back("store_board_key(" + std::to_string(key.size()) + ")");
        return store_succeeds;
    }
    void acquire_channels() override { effects.push_back("acquire"); }
    void release_channels() override { effects.push_back("release"); }
    bool send(const std::vector<uint8_t> &frame) override {
        effects.push_back("send(" + std::to_string(frame.size()) + ")");
        return transport_takes;
    }
    bool build_frame(uint8_t type, const std::vector<uint8_t> &body,
                     std::vector<uint8_t> &out) override {
        effects.push_back("build(" + std::to_string(type) + "," +
                          std::to_string(body.size()) + ")");
        if (!frame_builds) return false;
        /* A frame is longer than its body; a distinct size is what lets the
           send below be checked against the *built* frame rather than the
           body that went into it. */
        out.assign(body.size() + 4, 0xAB);
        return true;
    }
    void note_sent() override { effects.push_back("note_sent"); }
    void note_send_refused() override { effects.push_back("note_send_refused"); }
    void show_state(dh_helper_state state) override {
        effects.push_back("show_state(" + std::to_string(static_cast<int>(state)) + ")");
    }
    void deliver_text(const std::vector<uint8_t> &utf8) override {
        /* Byte by byte with an explicit cast rather than the iterator-pair
           std::string constructor: that one converts unsigned char to char
           inside the STL, and MSVC reads this test at /W4 with warnings as
           errors. */
        std::string text;
        text.reserve(utf8.size());
        for (uint8_t byte : utf8) text.push_back(static_cast<char>(byte));
        effects.push_back("deliver_text(" + text + ")");
    }
    void schedule_retry(uint32_t after_ms) override {
        effects.push_back("retry(" + std::to_string(after_ms) + ")");
    }
    std::vector<ClipOutput> clip_policy_changed(uint8_t flags) override {
        effects.push_back("clip_policy(" + std::to_string(flags) + ")");
        return policy_reply;
    }
    void log(const std::string &message) override { effects.push_back("log: " + message); }

    /* Whether the run so far contains a line beginning with `prefix`. Logs are
       matched by prefix rather than in full: what is being claimed is that the
       right thing was said, not the exact wording of a sentence that will be
       reworded. */
    bool logged(const std::string &prefix) const {
        for (const std::string &effect : effects)
            if (effect.rfind("log: " + prefix, 0) == 0) return true;
        return false;
    }

    /* Whether anything at all reached this computer's clipboard. */
    bool wrote_to_the_clipboard() const {
        for (const std::string &effect : effects)
            if (effect.rfind("deliver_text(", 0) == 0) return true;
        return false;
    }

    bool did(const std::string &effect) const {
        for (const std::string &done : effects)
            if (done == effect) return true;
        return false;
    }

    /* Where in the run an effect happened, or -1. For the two claims that are
       about order. */
    int index_of(const std::string &effect) const {
        for (size_t i = 0; i < effects.size(); ++i)
            if (effects[i] == effect) return static_cast<int>(i);
        return -1;
    }
};

/* ------------------------------------------------------------------ census */
/*
 * Every output kind, and the effect it must reach.
 *
 * Neither switch has a `default:`, which is the point of them: a kind added to
 * `HelperSession` or `ClipService` and forgotten stops this file compiling —
 * /W4 /WX under MSVC and -Werror=switch elsewhere — so a new kind fails the
 * suite rather than falling through the shim in silence. The names are what
 * the failure messages below quote.
 */
static const char *effect_named_by(Output::Kind kind) {
    switch (kind) {
    case Output::Kind::StoreBoardKey: return "the board key is stored";
    case Output::Kind::OpenChannels: return "the channels are acquired";
    case Output::Kind::CloseChannels: return "the channels are released";
    case Output::Kind::Send: return "the frame goes to the transport";
    case Output::Kind::State: return "the tray is shown the state";
    case Output::Kind::Retry: return "a retry is scheduled";
    case Output::Kind::Note: return "the note is logged";
    case Output::Kind::ClipPolicy: return "the clipboard service is told";
    }
    return "";
}

static const char *effect_named_by(ClipOutput::Kind kind) {
    switch (kind) {
    case ClipOutput::Kind::Send: return "the body is built into a frame and sent";
    case ClipOutput::Kind::Deliver: return "the payload reaches this computer's clipboard";
    case ClipOutput::Kind::Note: return "the note is logged";
    case ClipOutput::Kind::ProtocolError: return "the connection is dropped";
    }
    return "";
}

/* --------------------------------------------------------- session outputs */

static void store_board_key_reaches_the_secret_store() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    Output output;
    output.kind = Output::Kind::StoreBoardKey;
    output.bytes = std::vector<uint8_t>(64, 0x11);
    dispatch.apply(output);

    CHECK(recorder.did("store_board_key(64)"), effect_named_by(Output::Kind::StoreBoardKey));
    CHECK(!recorder.logged("paired, but"), "a key that was stored says nothing about pairing");
}

/* A key that cannot be written is pairing that will not survive a restart, and
   silence there is how a helper that is a stranger again after a reboot looks
   like the board forgetting. */
static void a_refused_board_key_is_said_out_loud() {
    Recorder recorder;
    recorder.store_succeeds = false;
    OutputDispatch dispatch(recorder);

    Output output;
    output.kind = Output::Kind::StoreBoardKey;
    output.bytes = std::vector<uint8_t>(64, 0x11);
    dispatch.apply(output);

    CHECK(recorder.logged("paired, but the board key could not be stored"),
          "a board key that could not be written is reported");
}

static void the_channel_outputs_reach_the_transport() {
    Recorder open;
    Output open_output;
    open_output.kind = Output::Kind::OpenChannels;
    OutputDispatch(open).apply(open_output);
    CHECK(open.did("acquire"), effect_named_by(Output::Kind::OpenChannels));

    Recorder close;
    Output close_output;
    close_output.kind = Output::Kind::CloseChannels;
    OutputDispatch(close).apply(close_output);
    CHECK(close.did("release"), effect_named_by(Output::Kind::CloseChannels));
}

static void a_sent_frame_charges_the_idle_timer() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    Output output;
    output.kind = Output::Kind::Send;
    output.bytes = std::vector<uint8_t>(32, 0x22);
    dispatch.apply(output);

    CHECK(recorder.did("send(32)"), effect_named_by(Output::Kind::Send));
    CHECK(recorder.did("note_sent"), "a frame the transport took charges the idle timer");
    CHECK(!recorder.did("note_send_refused"), "a frame that went out is not counted as refused");
}

/*
 * #107, in one test. Charging the idle timer for a frame the transport refused
 * buys a full interval of silence the helper has not earned, and the board
 * evicts after three of them.
 */
static void a_refused_frame_is_counted_and_said_out_loud() {
    Recorder recorder;
    recorder.transport_takes = false;
    OutputDispatch dispatch(recorder);

    Output output;
    output.kind = Output::Kind::Send;
    output.bytes = std::vector<uint8_t>(32, 0x22);
    dispatch.apply(output);

    CHECK(!recorder.did("note_sent"), "a refused frame does not charge the idle timer");
    CHECK(recorder.did("note_send_refused"), "a refused frame is counted");
    CHECK(recorder.logged("a session frame was not taken by the transport"),
          "a refused frame is distinguishable from a quiet link");
}

static void a_state_reaches_the_tray_in_words() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    Output output;
    output.kind = Output::Kind::State;
    output.state = DH_HELPER_DEVICE_ABSENT;
    dispatch.apply(output);

    /* The literal 5, not the enumerator spelled back out: an expected value
       computed the way the code computes it agrees with the code by
       construction and can never disagree with it. DH_HELPER_DEVICE_ABSENT is
       the core's fifth state (dh_helper.h), and a renumbering that moved it
       would be a wire change this should notice. */
    CHECK(recorder.did("show_state(5)"), effect_named_by(Output::Kind::State));
    CHECK(recorder.logged("state: "), "the state is logged in this helper's own words");
    CHECK(!recorder.logged("the core reported state"),
          "a state this helper has words for is not reported as unknown");
}

/* #119: a state added to the core and left out of words.h would otherwise be
   shown to nobody, reading exactly like the quiet state. */
static void a_state_with_no_words_says_so() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    Output output;
    output.kind = Output::Kind::State;
    output.state = static_cast<dh_helper_state>(200);
    dispatch.apply(output);

    CHECK(recorder.logged("the core reported state 200, which this helper has no words for"),
          "a state with no words is named rather than shown as quiet");
    CHECK(recorder.logged("state: (nothing to report)"),
          "a state with no words still reaches the log");
    CHECK(recorder.did("show_state(200)"), "the tray is still told, and decides for itself");
}

/* The board is the single source of truth for the policy, so the output has to
   reach the service that honours it — and whatever that service says in reply
   has to be carried out in the same pass. */
static void a_clip_policy_reaches_the_service_and_its_reply_is_carried_out() {
    Recorder recorder;
    ClipOutput reply;
    reply.kind = ClipOutput::Kind::Note;
    reply.note = "the far end may no longer receive";
    recorder.policy_reply = {reply};
    OutputDispatch dispatch(recorder);

    Output output;
    output.kind = Output::Kind::ClipPolicy;
    output.clip_flags = 0x01;
    dispatch.apply(output);

    CHECK(recorder.did("clip_policy(1)"), effect_named_by(Output::Kind::ClipPolicy));
    CHECK(recorder.logged("the far end may no longer receive"),
          "what the clipboard service answers is carried out, not dropped");
}

static void a_retry_is_handed_to_the_run_loop() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    Output output;
    output.kind = Output::Kind::Retry;
    output.retry_after_ms = 4000;
    dispatch.apply(output);

    CHECK(recorder.did("retry(4000)"), effect_named_by(Output::Kind::Retry));
}

static void a_session_note_is_logged() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    Output output;
    output.kind = Output::Kind::Note;
    output.note = "the listener was detected 4 times in 10000ms";
    dispatch.apply(output);

    CHECK(recorder.logged("the listener was detected 4 times in 10000ms"),
          effect_named_by(Output::Kind::Note));
}

/* ------------------------------------------------------- clipboard outputs */

static void a_clipboard_body_is_built_into_a_frame_and_sent() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    ClipOutput output;
    output.kind = ClipOutput::Kind::Send;
    output.type = 0x40;
    output.bytes = std::vector<uint8_t>(16, 0x33);
    dispatch.emit(output);

    CHECK(recorder.did("build(64,16)"), "the body goes through the session's counter space");
    CHECK(recorder.did("send(20)"),
          "the *built frame* is what reaches the transport, not the body");
    CHECK(recorder.did("note_sent"), effect_named_by(ClipOutput::Kind::Send));
}

/* No session means no counter to send under. Nothing goes out, and nothing is
   charged for a beat that never happened. */
static void a_clipboard_frame_with_no_session_is_dropped_loudly() {
    Recorder recorder;
    recorder.frame_builds = false;
    OutputDispatch dispatch(recorder);

    ClipOutput output;
    output.kind = ClipOutput::Kind::Send;
    output.type = 0x40;
    output.bytes = std::vector<uint8_t>(16, 0x33);
    dispatch.emit(output);

    CHECK(recorder.logged("a clipboard frame could not be built"),
          "a frame with no session to carry it is reported");
    CHECK(!recorder.did("send(20)"), "nothing is handed to the transport");
    CHECK(!recorder.did("note_sent"), "the idle timer is not charged");
}

/* #132: dropped in silence, a frame the transport would not take is
   indistinguishable from one lost on the wire. */
static void a_refused_clipboard_frame_is_counted_and_said_out_loud() {
    Recorder recorder;
    recorder.transport_takes = false;
    OutputDispatch dispatch(recorder);

    ClipOutput output;
    output.kind = ClipOutput::Kind::Send;
    output.type = 0x40;
    output.bytes = std::vector<uint8_t>(16, 0x33);
    dispatch.emit(output);

    CHECK(!recorder.did("note_sent"), "a refused clipboard frame does not charge the idle timer");
    CHECK(recorder.did("note_send_refused"), "a refused clipboard frame is counted");
    CHECK(recorder.logged("a clipboard frame of type 64 was not taken by the transport"),
          "the refusal names the message type");
}

static void a_text_payload_reaches_this_computers_clipboard() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    ClipOutput output;
    output.kind = ClipOutput::Kind::Deliver;
    output.payload_kind = static_cast<uint8_t>(ClipKind::Text);
    const std::string text = "hello";
    output.bytes.assign(text.begin(), text.end());
    dispatch.emit(output);

    CHECK(recorder.did("deliver_text(hello)"), effect_named_by(ClipOutput::Kind::Deliver));
}

/* Images are #55 and files are #56. Until then an arriving one is named rather
   than written as if it were text. */
static void a_payload_this_slice_cannot_write_is_named() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    ClipOutput output;
    output.kind = ClipOutput::Kind::Deliver;
    output.payload_kind = static_cast<uint8_t>(ClipKind::Png);
    output.bytes = std::vector<uint8_t>(8, 0x44);
    dispatch.emit(output);

    CHECK(recorder.logged("a payload of kind 1 arrived"), "the unwritable kind is named");
    CHECK(!recorder.wrote_to_the_clipboard(), "nothing is written to the clipboard");
}

static void a_clipboard_note_is_logged() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    ClipOutput output;
    output.kind = ClipOutput::Kind::Note;
    output.note = "offer superseded by a newer copy";
    dispatch.emit(output);

    CHECK(recorder.logged("offer superseded by a newer copy"),
          effect_named_by(ClipOutput::Kind::Note));
}

/*
 * #148/#149's teardown, which was the question this whole ticket came from:
 * an authenticated identity conflict drops the connection. Logged *before* the
 * release, so the reason survives in the log that the drop then explains.
 */
static void a_protocol_error_drops_the_connection() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    ClipOutput output;
    output.kind = ClipOutput::Kind::ProtocolError;
    output.note = "a second helper claims this seal";
    dispatch.emit(output);

    CHECK(recorder.did("release"), effect_named_by(ClipOutput::Kind::ProtocolError));
    CHECK(recorder.logged("clipboard protocol error: a second helper claims this seal"),
          "the conflict is named in the log");
    const int said = recorder.index_of("log: clipboard protocol error: a second helper claims "
                                       "this seal; dropping the connection");
    CHECK(said >= 0 && said < recorder.index_of("release"),
          "the reason is logged before the connection goes");
}

/* A batch is carried out in order, and one output's effect does not swallow
   the next. */
static void a_batch_is_carried_out_in_order() {
    Recorder recorder;
    OutputDispatch dispatch(recorder);

    ClipOutput note;
    note.kind = ClipOutput::Kind::Note;
    note.note = "first";
    ClipOutput error;
    error.kind = ClipOutput::Kind::ProtocolError;
    error.note = "second";
    dispatch.emit({note, error});

    CHECK(recorder.index_of("log: first") == 0, "the first output is carried out first");
    CHECK(recorder.did("release"), "the second output is carried out too");
}

int main() {
    store_board_key_reaches_the_secret_store();
    a_refused_board_key_is_said_out_loud();
    the_channel_outputs_reach_the_transport();
    a_sent_frame_charges_the_idle_timer();
    a_refused_frame_is_counted_and_said_out_loud();
    a_state_reaches_the_tray_in_words();
    a_state_with_no_words_says_so();
    a_clip_policy_reaches_the_service_and_its_reply_is_carried_out();
    a_retry_is_handed_to_the_run_loop();
    a_session_note_is_logged();

    a_clipboard_body_is_built_into_a_frame_and_sent();
    a_clipboard_frame_with_no_session_is_dropped_loudly();
    a_refused_clipboard_frame_is_counted_and_said_out_loud();
    a_text_payload_reaches_this_computers_clipboard();
    a_payload_this_slice_cannot_write_is_named();
    a_clipboard_note_is_logged();
    a_protocol_error_drops_the_connection();
    a_batch_is_carried_out_in_order();

    if (failures) {
        std::printf("%d output dispatch check(s) failed\n", failures);
        return 1;
    }
    std::printf("output dispatch checks passed\n");
    return 0;
}
