// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_STREAM_SWITCH_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_STREAM_SWITCH_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#ifdef OBR_HAVE_GSTREAMER
#include <gst/gst.h>
#endif

namespace standby
{

// The codec whose elementary-stream framing a switch has to read to tell a
// keyframe buffer from a mid-GOP one. A separate small enum from slate_codec
// on purpose: this code only ever reads a NAL/OBU header, and reading the same
// byte as the wrong codec is how a P-frame is mistaken for an IDR.
enum class es_frame_codec : std::uint8_t
{
  h264,
  h265,
  av1
};

// How the NALs of an h264/h265 access unit are delimited in the buffer that
// reaches the selector: Annex-B start codes (byte-stream) or 4-byte lengths
// (avc / avc3). AV1 ignores this (it is OBU framing).
enum class es_nal_framing : std::uint8_t
{
  annexb,
  length_prefixed
};

// True when the access unit in `data` begins a keyframe -- i.e. a decoder can
// start here without reference frames. The caller guarantees the buffer is AU
// aligned (the switch pipeline is parsed with alignment=au), because the answer
// is about the buffer boundary that the switch acts on.
//
// What is keyed on, per codec:
//  - h264: an SPS (NAL type 7) or an IDR slice (NAL type 5) anywhere in the AU.
//    Access-unit delimiters (type 9) and SEI are skipped over rather than
//    treated as "the first NAL", because x264enc and the VA encoders emit an
//    AUD before the SPS and a check on the literal first NAL would then never
//    fire.
//  - h265: an IRAP picture (NAL types 16..23: BLA/IDR/CRA), or a VPS (32) /
//    SPS (33) parameter set.
//  - av1: an OBU_SEQUENCE_HEADER, as av1parse emits ahead of a keyframe; when
//    the stream carries no sequence header the first frame OBU's uncompressed
//    header is read (show_existing_frame == 0 and frame_type == KEY_FRAME).
//
// Pure and dependency-free so the predicate can be exercised directly.
auto au_starts_keyframe(es_frame_codec codec, es_nal_framing framing,
                        const std::uint8_t* data, std::size_t len) -> bool;

// The NAL type of the first picture NAL (slice) in an h264/h265 AU, or -1 when
// there is none. Only used for reporting and by the test that records the type
// of the buffer each switch landed on.
auto first_picture_nal_type(es_frame_codec codec, es_nal_framing framing,
                            const std::uint8_t* data, std::size_t len) -> int;

#ifdef OBR_HAVE_GSTREAMER

// The ordering contract between the two switches of one session.
//
// Video and audio sit on separate input-selectors, so the audio switch cannot
// see the video selector through GStreamer. Without this state the audio
// selector would cross at its next buffer while the video selector is still
// waiting for an IDR, and the outputs would carry live sound over the slate
// picture for a whole GOP. The video switch publishes the branch it has
// actually reached and calls into the audio switch the moment it crosses; the
// audio switch may only follow that event. Audio has no keyframe of its own, so
// this is the only gate it needs.
//
// The audio side is driven by the event rather than by a probe on the audio
// branch: an input-selector's inactive sink pad returns NOT_LINKED, so a branch
// that is not selected may stop delivering buffers entirely (a queue upstream
// latches that flow error). Waiting for "the next audio buffer" on a branch
// that is not yet selected is therefore not a reliable trigger; the video
// switch crossing is.
struct switch_gate
{
  // sink_0 is the selector's default pad, so a freshly built session starts
  // here before anything has been switched.
  std::atomic<bool> video_on_live {true};

  std::mutex mutex;                 // guards `audio`
  class stream_switch* audio = nullptr;  // registered by the audio switch
};

// Per-stream switch configuration.
//
// audio: no keyframe concept -- an audio switch may land on any frame boundary,
//   but it must not precede the video switch. Pass the session's switch_gate to
//   order it; without a gate the audio switch flips at once.
// wait_for_keyframe: only meaningful for video; false for every session whose
//   outputs all transcode (the encoder re-encodes from a decoded frame, so a
//   mid-GOP join costs nothing) and for audio.
// gate: shared with the other switch of the session. The video switch resets
//   it on attach; the audio switch reads it.
// deadline_ms: how long a pending switch may wait for its event (a live IDR, or
//   the video switch crossing for the audio) before it is reported degraded.
//   The pending switch is not abandoned -- it still applies when the event
//   comes -- but an operator gets a log line instead of a silent, indefinite
//   wait.
// live_caps / slate_caps: the caps each branch is built with, used to verify
//   equality at attach when the branches have not negotiated yet (attach before
//   PLAYING). Once negotiated, the pads' real caps win.
struct switch_config
{
  bool audio = false;
  bool wait_for_keyframe = true;
  std::shared_ptr<switch_gate> gate;
  std::int64_t deadline_ms = 2000;
  std::string live_caps;
  std::string slate_caps;
};

// stream_switch -- the slate/live decision, wrapped around an input-selector
// that is already part of a pipeline built from a string.
//
// The pipeline must contain `input-selector name=sel` with request pads
// sink_0 (live) and sink_1 (slate). The selector is used as transport only:
// its sync/segment behaviour is not relied on, the re-stamper downstream is
// authoritative for time.
//
// What it guarantees:
//  - set_slate(true) asks for sink_1 and moves the selector at once. The slate
//    ES is all-intra (render.cpp pins a keyframe on every frame), so whatever
//    access unit the loop has reached is independently decodable and the first
//    buffer downstream is never a P-frame with no references. Gating this on a
//    keyframe probe of the slate pad is not an option: an input-selector
//    returns a flow error for a pad it is not presenting, which stops the slate
//    appsrc until that pad is selected, so the probe would never see the
//    keyframe it is waiting for.
//  - set_slate(false) flips at once when wait_for_keyframe is false, and
//    otherwise at the next live buffer that BEGINS a keyframe. The flip is
//    performed inside a buffer probe on the sink pad, before the selector
//    chains that buffer, so the keyframe itself is the first buffer the
//    downstream sees -- never a buffer mid-access-unit.
//  - An audio switch never precedes its video switch: it follows the shared
//    switch_gate, so the video selector crosses first and the audio selector is
//    moved when it does.
//  - A pending switch that does not get its event within deadline_ms is
//    reported degraded. It stays pending and still applies later; the
//    degraded() flag and the log line say the outputs are still on the old
//    branch, which is the truth the panel has to show.
//  - A caps mismatch between the branches refuses the attachment (attach
//    returns false, caps_ok() is false, the reason is logged loudly). A caps
//    change at the selector is a muxer renegotiation and a split video; it is
//    counted (caps_changes()) rather than tolerated.
//
// Thread safety: set_slate() may be called from the mode-controller thread
// while the streaming threads run. Element property changes and the pending
// flags are serialised by an internal mutex; the probes take the same mutex for
// the short part that touches state and do the per-buffer keyframe check
// outside it.
class stream_switch
{
public:
  using log_fn = std::function<void(const std::string&)>;

  explicit stream_switch(log_fn log = {});
  ~stream_switch();

  stream_switch(const stream_switch&) = delete;
  auto operator=(const stream_switch&) -> stream_switch& = delete;
  stream_switch(stream_switch&&) = delete;
  auto operator=(stream_switch&&) -> stream_switch& = delete;

  // Find `sel`, verify the two branches' caps agree and install the probes.
  // Returns false (and leaves the switch un-attached) when the selector or its
  // pads are missing, when the caps cannot be verified, or when they differ.
  // Safe to call before the pipeline reaches PLAYING; the configured caps are
  // used until the pads negotiate their own.
  auto attach(GstElement* pipeline, const switch_config& cfg) -> bool;

  // Remove the probes and drop every reference. After detach() the selector
  // keeps whatever pad is active; a later attach() re-verifies the caps.
  auto detach() -> void;

  // Ask for the slate (true) or live (false). See the class comment for the
  // keyframe gate; slate_active() reports where the selector actually is, so a
  // pending request reads as "still on the other branch" until the event.
  auto set_slate(bool slate) -> void;

  // Check the pending switch's deadline. Called from the stage's feed thread so
  // a degraded wait is reported even when one branch has gone silent (no
  // buffers arrive to run the probe). Cheap and idempotent.
  auto tick() -> void;

  [[nodiscard]] auto slate_active() const noexcept -> bool
  {
    return m_slate_active.load(std::memory_order_acquire);
  }

  // True once a pending switch has waited longer than deadline_ms.
  [[nodiscard]] auto degraded() const noexcept -> bool
  {
    return m_degraded.load(std::memory_order_acquire);
  }

  // Number of degraded reports emitted for this switch. A pending switch is
  // reported once per episode, not once per buffer.
  [[nodiscard]] auto deadline_reports() const noexcept -> std::uint64_t
  {
    return m_deadline_reports.load(std::memory_order_relaxed);
  }

  // Caps events seen on the selector's src pad that actually CHANGE the caps.
  // A selector re-sends the newly activated pad's sticky caps event on every
  // switch; those identical re-sends are not a renegotiation and are not
  // counted. Anything counted here is a real caps change at the muxer.
  [[nodiscard]] auto caps_changes() const noexcept -> std::uint64_t
  {
    return m_caps_changes.load(std::memory_order_relaxed);
  }

  [[nodiscard]] auto caps_ok() const noexcept -> bool
  {
    return m_caps_ok.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto attached() const noexcept -> bool
  {
    return m_attached.load(std::memory_order_acquire);
  }

private:
  enum class transition : std::uint8_t
  {
    none,
    to_live,
    to_slate,
    waiting,
    waiting_slate
  };

  // Requires m_mutex.
  auto apply_desired_locked() -> transition;
  // Requires m_mutex. Moves the selector and publishes the branch to the gate.
  auto flip_locked(bool to_live) -> void;
  // Requires m_mutex. True when the audio switch may follow the video switch
  // to the current target, i.e. the video selector is already on that branch.
  auto audio_gate_open_locked() const -> bool;
  // Called on the video switch once it has crossed, outside m_mutex. Hands the
  // event to the audio switch registered in the gate.
  auto follow_audio(bool to_live) -> void;
  // Called by the video switch (through the gate) when it has crossed. Moves
  // this audio selector if it is still waiting for that direction.
  auto video_crossed(bool to_live) -> void;
  auto arm_deadline_locked() -> void;
  // Requires m_mutex. Returns the degraded message the first time the pending
  // switch passes deadline_ms, else empty.
  auto check_deadline_locked() -> std::string;
  auto buffer_starts_keyframe(GstBuffer* buf) const -> bool;
  auto note_caps(GstCaps* caps) -> void;
  auto note(const std::string& message) const -> void;

  static auto branch_probe_cb(GstPad* pad, GstPadProbeInfo* info, gpointer user)
      -> GstPadProbeReturn;
  static auto src_probe_cb(GstPad* pad, GstPadProbeInfo* info, gpointer user)
      -> GstPadProbeReturn;
  auto on_branch_buffer(GstPad* pad, GstBuffer* buf) -> void;
  auto maybe_switch_to_live(GstBuffer* buf) -> void;

  log_fn m_log;

  mutable std::mutex m_mutex;  // guards the element refs and the transition
  GstElement* m_sel = nullptr;
  GstPad* m_live_pad = nullptr;
  GstPad* m_slate_pad = nullptr;
  GstPad* m_src_pad = nullptr;
  guint m_live_probe = 0;
  guint m_src_probe = 0;

  switch_config m_cfg;
  std::int64_t m_deadline_ms = 2000;
  std::shared_ptr<switch_gate> m_gate;
  es_frame_codec m_codec = es_frame_codec::h264;
  es_nal_framing m_framing = es_nal_framing::annexb;
  bool m_desired_slate = true;  // a session starts on the slate

  std::atomic<bool> m_slate_active {false};  // the selector's default is sink_0
  std::atomic<bool> m_pending_live {false};
  std::atomic<bool> m_pending_slate {false};
  std::atomic<bool> m_attached {false};
  std::atomic<bool> m_caps_ok {false};
  std::atomic<bool> m_degraded {false};
  std::atomic<std::uint64_t> m_deadline_reports {0};
  std::atomic<std::uint64_t> m_caps_changes {0};

  // Guarded by m_mutex.
  std::chrono::steady_clock::time_point m_pending_since {};
  bool m_deadline_reported = false;

  std::mutex m_caps_mutex;  // guards m_last_caps (probe vs status reader)
  GstCaps* m_last_caps = nullptr;
};

#endif  // OBR_HAVE_GSTREAMER

}  // namespace standby

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_STREAM_SWITCH_H
