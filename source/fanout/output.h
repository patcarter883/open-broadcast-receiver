// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_OUTPUT_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_OUTPUT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "fanout/es_ring.h"
#include "fanout/frame_ring.h"
#include "fanout/ring.h"
#include "lib/lib.h"

// Timing of the transit-buffer watchdog (see wait_appsrc_drain). A publish
// attempt has two phases with different failures: reaching publish, where a
// held buffer is the RTMP handshake and publish exchange in flight, and
// publishing, where a held buffer is a sink that stopped draining. The connect
// window is longer than the mid-stream stall window because it has to cover a
// handshake that a local sink completes in 6-7 s and a platform ingest can take
// several seconds longer.
namespace output_timing
{
inline constexpr auto connect_timeout = std::chrono::seconds(20);
inline constexpr auto stall_timeout = std::chrono::seconds(5);
}  // namespace output_timing

// What a full transit buffer means at the moment the feed thread finds it.
enum class drain_verdict : std::uint8_t
{
  keep_waiting,
  stalled,                 // publishing, then the sink stopped draining
  publish_not_established  // the connect window closed without a publish
};

// Classify a transit buffer that has been pinned at its limit for `held`.
// `published` is true once this attempt has confirmed a publish, i.e. the sink
// completed a transfer of media to the server.
auto classify_drain_hold(std::chrono::milliseconds held, bool published)
    -> drain_verdict;

// Which compressed elementary streams an output consumes when the standby slate
// is enabled (receiver_config.standby.enabled, schema_version 5).
//
// With the slate on, the shared standby stage parses the ingest ONCE and
// publishes whole access units into an es_ring per stream (video, audio). Every
// output then reads compressed AUs from those rings instead of being handed the
// transport stream: a copy output remuxes the AUs into its own container, and
// the shared decoder parses/decodes the video AUs directly. That is what lets
// the slate keep every destination fed while no live media is arriving, without
// each output running its own demux over a TS that may be the slate's.
//
// `enabled == false` is today's path, byte for byte: every output reads the
// ts_ring and demuxes it exactly as before. Nothing in the ring path changes
// because this struct exists.
struct es_input_spec
{
  bool enabled = false;
  // The DECLARED ingest codec (/start source.codec): it selects the parser in
  // front of the muxer or decoder. It is not a guess about the AUs.
  codec video_codec = codec::h264;
  // True when an audio es_ring exists for this session, i.e. the ingest carried
  // an audio stream and the stage published it. A video-only session leaves the
  // muxer's audio pad unrequested, exactly as the ts path does.
  bool audio = false;
  auto operator==(const es_input_spec&) const -> bool = default;
};

// The rings a standby-enabled session hands one output. Both null is the
// disabled path (a ts_ring consumer, no ES mode). main passes these only when
// standby_config.enabled is set; the later stage wiring owns the rings.
struct es_sources
{
  es_ring* video = nullptr;
  es_ring* audio = nullptr;
  codec video_codec = codec::h264;

  [[nodiscard]] auto enabled() const noexcept -> bool
  {
    return video != nullptr;
  }
  [[nodiscard]] auto spec() const noexcept -> es_input_spec
  {
    return es_input_spec {enabled(), video_codec, audio != nullptr};
  }
};

// Probe data for the destination sink's sink pad: the publish counters the pad
// probe increments. Pointers, so the probe needs no allocation and stays valid
// for the output's lifetime.
struct sink_publish_counters
{
  std::atomic<uint64_t>* bytes;
  std::atomic<uint64_t>* frames;
};

// output — one self-contained fan-out destination (TRANSPORT_PROFILE §3.2):
// its own small GstPipeline, its own bus handling, its own state machine and
// backoff, its own ring cursor feeding its own appsrc. Errors are local by
// construction: a failed output is destroyed and rebuilt, never surgically
// unlinked; nothing here can touch another output or the ingest path.
//
// Reconnect policy (§3.6, FIXPLAN M1.4): every failure retries forever with
// 1 s → ×2 → 30 s-cap backoff — a live event must ride out a platform's
// 5-minute ingest outage without operator action. The only terminal state is
// rtmp_codec_unsupported (a non-H.264 elementary stream cannot enter FLV; it
// cannot self-heal). Auth-class failures are deliberately NOT terminal: many
// RTMP ingests close the TCP connection on a bad stream key, which is
// indistinguishable from a transient reset, so the receiver retries and the
// panel/agent raise "check your stream key" advisories on top of
// reconnects/last_error instead.
class output
{
public:
  enum class run_state : std::uint8_t
  {
    starting,
    running,
    reconnecting,
    error,   // terminal until the next /start
    stopped
  };

  using log_fn = std::function<void(const std::string&)>;
  // Re-resolve + re-vet the destination before a connect attempt, updating
  // `pinned_ip`. Supplied by main, which owns the egress policy; called on the
  // output's own thread. Returns false when the name does not resolve or every
  // A/AAAA record is refused, in which case the caller keeps the previous pin
  // rather than tearing the output down.
  using repin_fn = std::function<bool(output_config&)>;

  // cfg must already be validated + pinned (validate_config +
  // validate_and_pin_outputs). The rings must outlive this object. `frames`
  // is the shared decode stage's frame_ring, required for a transcode output
  // and unused (may be nullptr) for a copy-only one. `es` is the standby
  // stage's rings; empty means the ts_ring path this class has always taken.
  output(output_config cfg,
         ts_ring& ring,
         frame_ring* frames,
         log_fn log,
         repin_fn repin = {},
         es_sources es = {});
  ~output();
  output(const output&) = delete;
  auto operator=(const output&) -> output& = delete;
  output(output&&) = delete;
  auto operator=(output&&) -> output& = delete;

  // Attach a ring cursor and launch the lifecycle thread. Returns false if no
  // consumer slot is available (cannot happen within the k_max_outputs bound).
  auto start() -> bool;

  // Tear down: pipeline to NULL, detach the cursor, join the thread. Safe to
  // call twice; called by the destructor.
  auto stop() -> void;

  // ---- observability (atomics/snapshots only; /stats reads these) ----

  [[nodiscard]] auto id() const -> const std::string& { return m_cfg.id; }
  [[nodiscard]] auto proto() const -> output_proto { return m_cfg.type; }
  [[nodiscard]] auto transcode() const -> transcode_target
  {
    return m_cfg.transcode.target;
  }
  [[nodiscard]] auto state() const -> run_state
  {
    return m_state.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto state_name() const -> const char*;
  [[nodiscard]] auto reconnects() const -> uint64_t
  {
    return m_reconnects.load(std::memory_order_relaxed);
  }
  // Bytes the destination sink has taken off its sink pad — muxed protocol
  // payload on its way to the wire, not the frame payload queued into the
  // appsrc upstream of the encoder and muxer.
  [[nodiscard]] auto bytes_published_total() const -> uint64_t
  {
    return m_bytes_published.load(std::memory_order_relaxed);
  }
  // Media buffers handed to that sink pad (video and audio tags for flvmux).
  [[nodiscard]] auto frames_published_total() const -> uint64_t
  {
    return m_frames_published.load(std::memory_order_relaxed);
  }
  // Deprecated alias for bytes_published_total(), kept so existing readers get
  // the truthful figure instead of the old appsrc-push sum.
  [[nodiscard]] auto bytes_sent() const -> uint64_t
  {
    return bytes_published_total();
  }
  // Bytes pushed into the output's appsrc(s): every decoded frame (raw, before
  // encode) or transcoded/copied AU handed to the pipeline. Superset of what
  // leaves the box and useful only for feed diagnostics; never socket egress.
  [[nodiscard]] auto bytes_pushed() const -> uint64_t
  {
    return m_bytes_pushed.load(std::memory_order_relaxed);
  }
  [[nodiscard]] auto dropped_bytes() const -> uint64_t
  {
    return (m_consumer >= 0) ? m_ring.dropped_bytes(m_consumer) : 0;
  }
  // Whole access units this output never saw because its es_ring cursor was
  // lapped (standby-enabled path), video plus audio. The ts_ring path reports 0
  // here and accounts its loss through dropped_bytes(); the two are separate
  // units on purpose (a TS packet count is not an AU count), so /stats must
  // render whichever one the session's mode produces rather than add them.
  [[nodiscard]] auto dropped_aus() const -> uint64_t;
  // Chosen transcode encoder element name (e.g. "vah264enc"), or "" for a
  // copy-only output / before the first pipeline build.
  [[nodiscard]] auto encoder_name() const -> std::string;
  // Encoder target in kbps currently applied, or 0 while the encoder runs at
  // its own default (copy-only outputs always report 0). While a connect
  // attempt has not confirmed its publish this is the bootstrap rate; it moves
  // to the measured-link target on confirmation.
  [[nodiscard]] auto bitrate_kbps() const -> int
  {
    return m_applied_bitrate_kbps.load(std::memory_order_relaxed);
  }
  // Hand the latest measured link rate to this output. A transcode output
  // re-derives its encoder target from it (see transcode_bitrate_policy) and
  // re-targets the running encoder when the change is material; a copy-only
  // output ignores it. Called from the link-rate controller thread, never from
  // the pipeline's own.
  auto retarget_bitrate(int measured_link_kbps) -> void;
  // Whole-frame drops on this output's frame_ring cursor (transcode only).
  [[nodiscard]] auto frames_dropped() const -> uint64_t
  {
    return (m_frame_consumer >= 0 && m_frames != nullptr)
        ? m_frames->dropped_frames(m_frame_consumer)
        : 0;
  }
  [[nodiscard]] auto audio_dropped() const -> bool
  {
    return m_audio_dropped.load(std::memory_order_relaxed);
  }
  // Buffers the muxer's audio pad has received on this output. A linked pad
  // with a zero count is an audio branch that is negotiated but silent.
  [[nodiscard]] auto audio_frames() const -> uint64_t
  {
    return m_audio_frames.load(std::memory_order_relaxed);
  }
  // Seconds in state=running for the current connection, 0 otherwise.
  [[nodiscard]] auto connected_s() const -> int64_t;
  [[nodiscard]] auto last_error() const -> std::string;

  // A new reference to the current attempt's pipeline, or nullptr between
  // attempts (caller owns the reference). The local RTMP harness
  // (test/source/rtmp_publish_harness.cpp) uses it to probe the muxer's pads and
  // read element state: a sink pad that has taken buffers says the pipeline is
  // running, while the server's counter says the bytes arrived.
  [[nodiscard]] auto pipeline() const -> GstElement*;

  // Registry pre-check for /start. Returns the first unsatisfied element
  // requirement (base chain => element_unavailable; transcode chain =>
  // transcode_unavailable), or nullopt when every requirement is met. For a
  // transcode output the probe covers the decode -> convert -> encode chain
  // and is satisfied by ANY alternative element name present.
  static auto first_missing_element(output_proto proto,
                                    const transcode_config& transcode,
                                    codec in_codec)
      -> std::optional<missing_requirement>;

private:
  auto worker() -> void;
  // One connect attempt: build → play → feed until error/stop. Returns true
  // if the failure is retryable, false if terminal (state already set).
  auto run_once() -> bool;
  // Transcode feed step (video from frame_ring, AAC audio from ts_ring or the
  // audio es_ring). Returns false when the attempt should end (appsrc flushing
  // / stopping).
  auto feed_transcode(bool& video_caps_set) -> bool;
  // Copy feed step in ES mode: video AUs from the video es_ring into osrc and
  // audio AUs from the audio es_ring into asrc. Returns false when the attempt
  // should end (appsrc flushing).
  auto feed_es_copy() -> bool;
  // Push every audio AU the audio es_ring currently holds into `asrc`. Returns
  // false when the appsrc rejected a buffer (pipeline flushing).
  auto feed_es_audio() -> bool;
  // ES mode: link acaps to the muxer's audio pad, without a demux pad to wait
  // on. The audio es_ring is what says audio exists, so this runs at build time
  // (the ts path defers it to tsdemux's pad-added, where the TS is the evidence).
  auto attach_es_audio_branch() -> void;
  // Link `pad` to `element_name`'s static pad (or to a requested pad when the
  // pad name ends in %u). Returns true only when this call made the link.
  auto link_pad_to(GstPad* pad, const char* element_name, const char* pad_name)
      -> bool;
  // Set the encoded appsrc caps (raw NV12) from a decoded frame's metadata.
  auto set_video_caps(std::size_t frame_bytes,
                      const frame_ring::frame_meta& meta) -> void;
  // Report the caps arriving at the muxer's video pad, once per attempt:
  // profile, chroma format and bit depth are what a destination refuses first.
  auto log_muxer_input() -> void;
  // Transcode feed accounting: one line every k_feed_stats_period.
  auto report_feed() -> void;
  // Links the source TS's audio pad into the AAC branch and requests the
  // muxer's audio pad for it. Called from demux_pad_added(), i.e. only when the
  // TS actually carries an audio stream.
  auto attach_audio_branch(GstPad* demux_pad) -> void;
  // Sinks a demux src pad the output does not consume into the drop funnel, so
  // it cannot fail the pipeline with not-linked.
  auto sink_into_drop(GstPad* pad) -> void;
  // Copy-tier gate: holds the feed's ring cursor while the video appsrc is at
  // its cap, then defers to wait_appsrc_drain's verdict. Returns false when the
  // attempt should end (stopping, a stalled sink, or a connect that never
  // reached publish).
  auto gate_copy_appsrc() -> bool;
  // Blocks until the video appsrc is below `cap`, bounded by the connect window
  // before this attempt has published and by the stall window after. Returns
  // false when the attempt should end (stopping, a stalled sink, or a connect
  // that never reached publish).
  auto wait_appsrc_drain(guint64 cap) -> bool;
  // The encoder target this attempt should run at right now: the bootstrap rate
  // until publish is confirmed, the measured-link target afterwards.
  [[nodiscard]] auto current_target_kbps() const -> int;
  // Apply `target_kbps` to the running encoder when it differs materially from
  // the applied rate. Runs on the worker or link-controller thread; the encoder
  // is reached through a reference to the pipeline taken under m_pipe_mutex.
  auto apply_encoder_bitrate(int target_kbps) -> void;
  // First confirmation that this attempt is publishing: the sink has accepted
  // media past the handshake. Retires the connect-window exemption and ramps
  // the encoder from the bootstrap rate to the measured-link target.
  auto confirm_publish() -> void;
  static auto appsrc_drained(GstPad* pad, GstPadProbeInfo* info, gpointer self)
      -> GstPadProbeReturn;
  static auto muxer_first_buffer(GstPad* pad, GstPadProbeInfo* info,
                                 gpointer self) -> GstPadProbeReturn;
  // Reports the caps and the first buffer count on the muxer's audio pad. A
  // linked audio pad that never receives a buffer is indistinguishable from a
  // healthy one in the stats, and the caps (stream-format=raw + codec_data) are
  // what flvmux needed the branch to be built for.
  static auto muxer_audio_first_buffer(GstPad* pad, GstPadProbeInfo* info,
                                       gpointer self) -> GstPadProbeReturn;
  auto build_pipeline(std::string& err) -> bool;
  auto destroy_pipeline() -> void;
  auto set_state(run_state next) -> void;
  auto set_last_error(const std::string& err) -> void;
  auto log(const std::string& msg) const -> void;

  // Classify a bus error: returns true when the error is the terminal
  // rtmp_codec_unsupported class (caps/negotiation failure on an FLV path).
  auto is_codec_terminal(const GError* err, GstElement* src) const -> bool;

  // Retires the audio branch of a live attempt: the demux pad is moved to the
  // drop funnel and an untouched muxer audio pad is released. Video keeps
  // flowing, because audio is optional and video is not.
  auto degrade_audio_branch(std::string_view reason) -> void;

  static auto demux_pad_added(GstElement* demux, GstPad* pad, gpointer self)
      -> void;

  output_config m_cfg;
  ts_ring& m_ring;
  frame_ring* m_frames = nullptr;  // shared decode stage (transcode only)
  log_fn m_log;
  // Standby-stage rings (empty on the ts_ring path, which is every session
  // without standby_config.enabled).
  es_sources m_es;

  int m_consumer = -1;        // ts_ring cursor (copy video / transcode audio)
  int m_frame_consumer = -1;  // frame_ring cursor (transcode video)
  // es_ring cursors, ES mode only: video for every output, audio only when the
  // output's muxer takes audio (copy rtmp/rtmps, transcode rtmp/rtmps).
  int m_es_video_consumer = -1;
  int m_es_audio_consumer = -1;
  // One AU at a time, sized for the largest AU the ring can hold rather than
  // the 32 KiB TS chunk: an IDR at 4K is megabytes, and es_ring::read skips an
  // AU that does not fit its destination whole.
  std::vector<std::uint8_t> m_es_video_buf;
  std::vector<std::uint8_t> m_es_audio_buf;
  std::thread m_thread;
  std::atomic_bool m_stopping {false};

  GstElement* m_pipeline = nullptr;
  GstElement* m_appsrc = nullptr;        // video appsrc ("osrc")
  GstElement* m_audio_appsrc = nullptr;  // transcode audio appsrc ("asrc")
  GstBus* m_bus = nullptr;

  std::atomic<run_state> m_state {run_state::starting};
  std::atomic<uint64_t> m_reconnects {0};
  std::chrono::steady_clock::time_point m_run_started {};
  // Set once the muxer's audio pad has been requested for the source TS's
  // audio stream; guards against a second audio ES taking a second pad.
  std::atomic_bool m_audio_linked {false};
  std::atomic_bool m_first_output_logged {false};
  // When the first decoded frame reached the video appsrc; the "first published
  // frame" latency line and the audio-branch probe both measure from here, not
  // from the attempt's start. The output is built before any RIST payload
  // exists, so a pipeline-start clock retires a perfectly good audio branch
  // before its first buffer can arrive.
  std::chrono::steady_clock::time_point m_first_video_push {};
  bool m_muxer_input_logged {false};
  // Set when the muxer's audio pad has been reported with the caps it received,
  // so the report names the negotiated format (raw + codec_data) once.
  std::atomic_bool m_muxer_audio_logged {false};
  // What the sink pad itself received: the protocol element's input bytes and
  // buffer count. This is the publish truth /stats reports.
  std::atomic<uint64_t> m_bytes_published {0};
  std::atomic<uint64_t> m_frames_published {0};
  sink_publish_counters m_publish_probe {&m_bytes_published,
                                         &m_frames_published};
  // What the feed handed the output's appsrc(s): raw decoded frames before
  // encode, or copied/transcoded AUs. A strict superset of the sink pad count.
  std::atomic<uint64_t> m_bytes_pushed {0};
  std::atomic_bool m_audio_dropped {false};
  // Buffers the muxer's audio pad actually received. Caps on that pad are not
  // data; a linked pad with no buffers is how flvmux stalls without an error,
  // which is what decides whether a failed branch's muxer pad can still be
  // released.
  std::atomic<uint64_t> m_audio_frames {0};
  // Guards the audio branch's pads: attach runs on the demux streaming thread,
  // degrade on the output's worker thread.
  std::mutex m_audio_mutex;
  GstPad* m_audio_demux_pad = nullptr;  // demux audio pad -> aq (owned ref)
  GstPad* m_audio_mux_pad = nullptr;    // acaps src -> mux audio (owned ref)
  std::atomic<int64_t> m_running_since_ms {0};  // steady ms; 0 = not running

  // Transcode feed accounting, reported once every k_feed_stats_period. It
  // separates the two ways the feed loses frames: it never read them from the
  // ring (the appsrc cap held it back) or it read and pushed them and the
  // encoder could not keep up.
  struct feed_stats
  {
    uint64_t iters = 0;      // passes through feed_transcode
    uint64_t consumed = 0;   // frames read out of the frame ring
    uint64_t pushed = 0;     // buffers accepted by the video appsrc
    uint64_t blocked = 0;    // passes that found the appsrc at/over its cap
    double blocked_ms = 0;   // time spent sleeping for that cap
    double read_ms = 0;      // time inside frame_ring::read
    double push_ms = 0;      // time inside gst_app_src_push_buffer
    uint64_t level_peak = 0; // high-water mark of the appsrc level, bytes
    uint64_t encoder_in = 0; // buffers that arrived at the encoder's sink pad
    uint64_t muxer_out = 0;  // buffers the muxer emitted
    std::chrono::steady_clock::time_point since {};
  };
  feed_stats m_feed;
  // Buffers that reached the encoder and that the muxer emitted; the feed
  // report prints them per period so a pinned appsrc can be attributed to the
  // encoder or to the muxer/sink.
  std::atomic<uint64_t> m_encoder_in {0};
  std::atomic<uint64_t> m_muxer_out {0};
  // Buffers that arrived at the destination sink's sink pad. The first one is
  // held for the RTMP handshake and publish exchange; the second one can only
  // arrive once that exchange completed, so the count is the publish signal this
  // attempt's watchdog and bitrate ramp key off.
  std::atomic<uint64_t> m_sink_in {0};
  std::atomic_bool m_publish_confirmed {false};
  // Wakes the feed thread when a buffer leaves the video appsrc (installed as a
  // probe on its src pad) so backpressure is the pipeline's own pace.
  std::mutex m_feed_mutex;
  std::condition_variable m_feed_cv;

  // Raw-frame read buffer for the frame_ring cursor (capacity()-sized; the
  // ring insists a read destination hold a whole frame). Sized once in start().
  std::vector<uint8_t> m_frame_buf;

  mutable std::mutex m_err_mutex;
  std::string m_last_error;

  // Guards the pipeline pointer handed out by pipeline(); every other use of
  // m_pipeline is on the worker thread.
  mutable std::mutex m_pipe_mutex;

  mutable std::mutex m_enc_mutex;
  std::string m_encoder_name;

  // Re-resolve hook (may be empty: literal-IP destinations and unit tests).
  repin_fn m_repin;

  // Latest measured ingest rate handed in by the link-rate controller (0 = no
  // measurement yet) and the target actually written to the encoder (0 = the
  // encoder's own default). Both are read at pipeline build time, which is why
  // they are atomics rather than plain members.
  std::atomic<int> m_measured_link_kbps {0};
  std::atomic<int> m_applied_bitrate_kbps {0};
};

// Build the gst_parse_launch template for an output (TRANSPORT_PROFILE §3.5).
// `encoder` is the concrete transcode encoder element name chosen from the
// shared alternatives list; it is ignored (may be nullptr) for a copy-only
// config. `es` selects the standby-enabled shape: with `es.enabled` a copy
// output is fed compressed AUs and remuxes them itself (no tsparse, no tsdemux),
// and a transcode output's AAC audio comes from the audio es_ring instead of a
// per-output tsdemux. With `es.enabled == false` (the default) the string is
// exactly what it has always been.
// The template carries NO secrets and NO customer URLs: the sink is
// created unconfigured (name=osink) and its connect properties are set by the
// caller, so the string is loggable by definition. Exposed so the full-lane
// test can assert and parse-launch the exact strings used.
auto output_template(output_proto proto,
                     const transcode_config& transcode,
                     const char* encoder,
                     const es_input_spec& es = {}) -> std::string;

// The elements that make up an RTMP output's audio branch. A bus error whose
// source is one of these is retired with the branch rather than the attempt.
auto is_audio_branch_element(std::string_view element_name) -> bool;

// The RTMP audio branch (queue -> ... -> acaps) both RTMP templates use.
// `transcode` selects the re-encoding branch (decode, resample to the declared
// output format, encode) over the copy branch (aacparse passthrough). Exposed so
// the branch can be run on real MPEG-TS sources without restating it in a test.
auto rtmp_audio_branch(bool transcode) -> std::string;

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_OUTPUT_H
