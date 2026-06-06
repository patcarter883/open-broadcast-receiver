#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_LIB_LIB_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_LIB_LIB_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

// Forward declarations of the runtime components owned by app_context. Their
// full definitions live in their own modules; app_context is only ever
// destroyed in a translation unit (main.cpp) where the complete types are
// visible.
class rist_receive;
class restream;
class control_server;

// ---------------------------------------------------------------------------
// Enums — the integer order MUST stay byte-for-byte identical to the encoder's
// (open-broadcast-encoder/source/lib/lib.h). The control-plane JSON maps these
// strings to/from the same enum values; see docs/CONTRACT.md §3.
// ---------------------------------------------------------------------------

enum class codec : std::uint8_t
{
  h264,
  h265,
  av1
};

// Audio codecs the receiver can ingest. NOT part of the control-plane contract:
// it is *detected* from the incoming MPEG-TS, never declared by the encoder.
// The receiver passes AAC through to RTMP (aacparse); other codecs have no audio
// path (the source is expected to be AAC). See restream.
enum class audio_codec : std::uint8_t
{
  aac,
  opus,
  ac3,
  eac3,
  mp2
};

// ---------------------------------------------------------------------------
// RIST OOB telemetry back-channel (receiver -> encoder). Byte-for-byte
// identical to open-broadcast-encoder/source/lib/lib.h:53-59. The encoder
// rejects any OOB payload whose size != sizeof(wan_telemetry).
// ---------------------------------------------------------------------------

struct __attribute__((packed)) wan_telemetry
{
  uint8_t link_quality;     // 0..100, raw byte (no byte order)
  uint32_t worst_case_rtt;  // milliseconds, network byte order on the wire
};
static_assert(sizeof(wan_telemetry) == sizeof(uint8_t) + sizeof(uint32_t),
              "wan_telemetry must be exactly 5 bytes");

// ---------------------------------------------------------------------------
// Named defaults for configuration fields (CONTRACT §4 recommended values)
// ---------------------------------------------------------------------------

struct receiver_defaults
{
  static constexpr int bandwidth = 6000;
  // Recovery buffer floor must leave room for several retransmit rounds over a
  // high-RTT mobile link. The reorder hold-off must be a SMALL fraction of the
  // buffer (librist default 15 ms) — a large reorder value eats the recovery
  // window and effectively disables retransmission (no NACKs → unrecovered loss).
  static constexpr int buffer_min_ms = 1000;
  static constexpr int buffer_max_ms = 5000;
  static constexpr int rtt_min_ms = 40;
  static constexpr int rtt_max_ms = 500;
  static constexpr int reorder_buffer_ms = 30;
};

// Recommended defaults for the on-GPU re-encode (CONTRACT §4). The encoder's
// outputs[0].video block overrides these per /start; absent fields keep these.
struct reencode_defaults
{
  static constexpr int bitrate_kbps = 4300;
  static constexpr int width = 2560;
  static constexpr int height = 1440;
  static constexpr int gop_size = 120;
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct ingest_config
{
  std::string rist_listen = "rist://@[::]:5000";
  int bandwidth = receiver_defaults::bandwidth;
  int buffer_min = receiver_defaults::buffer_min_ms;
  int buffer_max = receiver_defaults::buffer_max_ms;
  int rtt_min = receiver_defaults::rtt_min_ms;
  int rtt_max = receiver_defaults::rtt_max_ms;
  int reorder_buffer = receiver_defaults::reorder_buffer_ms;
  auto operator==(const ingest_config&) const -> bool = default;
};

// How the receiver re-encodes the decoded video ON THE GPU and pushes a single
// H264 RTMP publish to the local restreaming package (datarhei/restreamer),
// which fans it out (codec copy) to YouTube/etc. Frames never leave CUDA memory
// between NVDEC and nvh264enc. Most fields come from the encoder's
// outputs[0].video over /start; rtmp_location + prefer_hw_decode are operator
// infrastructure (CLI). On stream2 only nvh264enc exists, so out_codec is pinned
// to h264. AAC audio is passed through (never decoded/re-encoded). See
// docs/CONTRACT.md and docs/GSTREAMER.md.
struct reencode_config
{
  bool reencode = true;               // false => H264 copy passthrough (no NVENC)
  codec out_codec = codec::h264;      // RTMP carries H264 only (pinned on stream2)
  int bitrate_kbps = reencode_defaults::bitrate_kbps;
  bool upscale = false;               // insert cudascale only when true
  int width = reencode_defaults::width;
  int height = reencode_defaults::height;
  int gop_size = reencode_defaults::gop_size;
  std::string preset = "low-latency-hq";  // nvh264enc preset (bare token)
  std::string rtmp_location =
      "rtmp://127.0.0.1:1935/blue.stream?token=2e2mWs72wmWKmnr";
  bool prefer_hw_decode = true;       // prefer NVDEC/VA/QSV over software decode
  auto operator==(const reencode_config&) const -> bool = default;
};

struct receiver_config
{
  int schema_version = 1;
  std::string session_id;
  ingest_config ingest;
  codec in_codec = codec::h264;  // the codec arriving over RIST (source.codec hint)
  reencode_config reencode;      // on-GPU encode + RTMP push (mixed CLI / /start)
  auto operator==(const receiver_config&) const -> bool = default;
};

// ---------------------------------------------------------------------------
// Runtime state (shared across the control, receive and restream threads)
// ---------------------------------------------------------------------------

struct receiver_state
{
  std::atomic_bool is_running {false};

  // Guards cfg, session_id, started_at, last_bus_error and the detected codecs.
  std::mutex mutex;
  receiver_config cfg;
  std::string session_id;
  std::chrono::steady_clock::time_point started_at;
  std::string last_bus_error;
  // Codecs detected off the live MPEG-TS once phase-1 detection settles; empty
  // until then. Surfaced (read-only) by GET /status.
  std::string detected_video;
  std::string detected_audio;

  // Telemetry mirror (also sent over RIST OOB). Lock-free for /status reads.
  std::atomic<int> link_quality {0};
  std::atomic<uint32_t> worst_rtt {0};
  std::atomic_bool have_peer {false};
};

struct app_context
{
  receiver_state state;
  std::unique_ptr<rist_receive> receive;
  std::unique_ptr<restream> restreamer;
  std::unique_ptr<control_server> control;
  std::string auth_token;  // empty => dev/no-auth mode
};

// ---------------------------------------------------------------------------
// Enum <-> string helpers (docs/CONTRACT.md §3)
// ---------------------------------------------------------------------------

auto to_string(codec cod) noexcept -> const char*;
auto to_string(audio_codec cod) noexcept -> const char*;

auto parse_codec(std::string_view str, codec& out) noexcept -> bool;

// Build the RIST listener URL the receiver hands to initReceiver. Mirrors the
// encoder's recovery params and appends timing-mode=1 (ARRIVAL). The base (scheme +
// "@host:port") is taken from ingest.rist_listen up to any '?'. Fixes the
// original ndi-rist-server missing-'&' bug. NOTE: it does NOT append a
// profile= URL param — librist's URL parser rejects it; the ADVANCED profile
// is set via RISTNetReceiverSettings.mProfile instead (see receive.cpp).
auto build_listener_url(const ingest_config& ingest) -> std::string;

// Parse the numeric port from a "rist://@[::]:PORT" / "rist://@host:PORT"
// listen URL. Returns -1 if it cannot be determined.
auto listen_port_from_url(const std::string& rist_listen) noexcept -> int;

// Parse "scheme://host:port[/...][?...]" into host + port (handles bracketed
// IPv6). Returns false if host/port cannot be determined.
auto parse_authority(const std::string& url,
                     std::string& host,
                     int& port) noexcept -> bool;

// ---------------------------------------------------------------------------
// Validation (pure: no GStreamer, no JSON). Field-shape/type checks live in the
// JSON parser (control.cpp); these are the cross-field/value rules of
// CONTRACT §4 that do not need the GStreamer registry.
// ---------------------------------------------------------------------------

struct validation_result
{
  bool ok = true;
  std::string error_code;  // e.g. "out_of_range", "bad_url"
  std::string field;       // e.g. "ingest.bandwidth"
  std::string message;
};

auto validate_config(const receiver_config& cfg) -> validation_result;

// True if `str` is safe to interpolate into a single-quoted gst_parse_launch
// property value (no quote-escape / control characters).
auto is_pipeline_safe(std::string_view str) noexcept -> bool;

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_LIB_LIB_H
