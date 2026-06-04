#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_LIB_LIB_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_LIB_LIB_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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

// Audio codecs the receiver can ingest. Unlike `codec` (video), this is NOT
// part of the control-plane contract: it is *detected* from the incoming
// MPEG-TS, never declared by the encoder. The output audio codec is always AAC
// (what RTMP/SRT/RIST muxers expect), so a non-AAC input is transcoded.
enum class audio_codec : std::uint8_t
{
  aac,
  opus,
  ac3,
  eac3,
  mp2
};

enum class encoder : std::uint8_t
{
  amd,
  qsv,
  nvenc,
  software
};

// Output transport for a restream destination. `rtmps` shares the RTMP/flvmux
// branch with a TLS URL; `srt`/`rist` use mpegtsmux.
enum class output_proto : std::uint8_t
{
  rtmp,
  rtmps,
  srt,
  rist
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
  static constexpr int video_bitrate_kbps = 4300;
  static constexpr int video_width = 2560;
  static constexpr int video_height = 1440;
  static constexpr int audio_bitrate_kbps = 128;
  static constexpr int latency_ms = 200;
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

// ---------------------------------------------------------------------------
// Configuration (deserialised from POST /start; see docs/CONTRACT.md §4)
// ---------------------------------------------------------------------------

struct video_disposition
{
  bool reencode = false;  // false => copy/passthrough
  codec out_codec = codec::h264;
  encoder enc = encoder::software;
  int bitrate_kbps = receiver_defaults::video_bitrate_kbps;
  bool upscale = false;
  int width = receiver_defaults::video_width;
  int height = receiver_defaults::video_height;
  auto operator==(const video_disposition&) const -> bool = default;
};

struct audio_disposition
{
  bool reencode = false;  // false => copy aac
  int bitrate_kbps = receiver_defaults::audio_bitrate_kbps;
  auto operator==(const audio_disposition&) const -> bool = default;
};

struct destination
{
  std::string id;
  output_proto proto = output_proto::rtmp;
  std::string url;
  std::string key_or_streamid;
  int latency_ms = receiver_defaults::latency_ms;  // srt only
  int sender_buffer = 0;                           // rist only (0 => omit)
  std::string cname;                               // rist only
  video_disposition video;
  audio_disposition audio;
  auto operator==(const destination&) const -> bool = default;
};

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

struct receiver_config
{
  int schema_version = 1;
  std::string session_id;
  ingest_config ingest;
  codec in_codec = codec::h264;  // the codec arriving over RIST (source.codec)
  std::vector<destination> destinations;
  auto operator==(const receiver_config&) const -> bool = default;
};

// ---------------------------------------------------------------------------
// Runtime state (shared across the control, receive and restream threads)
// ---------------------------------------------------------------------------

struct receiver_state
{
  std::atomic_bool is_running {false};

  // Guards cfg, session_id, started_at and last_bus_error.
  std::mutex mutex;
  receiver_config cfg;
  std::string session_id;
  std::chrono::steady_clock::time_point started_at;
  std::string last_bus_error;

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
auto to_string(encoder enc) noexcept -> const char*;
auto to_string(output_proto proto) noexcept -> const char*;

auto parse_codec(std::string_view str, codec& out) noexcept -> bool;
auto parse_encoder(std::string_view str, encoder& out) noexcept -> bool;
auto parse_output_proto(std::string_view str, output_proto& out) noexcept -> bool;

// Build the RIST listener URL the receiver hands to initReceiver. Mirrors the
// encoder's recovery params and appends timing-mode=2. The base (scheme +
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
  std::string error_code;  // e.g. "bad_url", "rtmp_codec_unsupported"
  std::string field;       // e.g. "outputs[0].url"
  std::string message;
};

auto validate_config(const receiver_config& cfg) -> validation_result;

// True if `str` is safe to interpolate into a single-quoted gst_parse_launch
// property value (no quote-escape / control characters).
auto is_pipeline_safe(std::string_view str) noexcept -> bool;

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_LIB_LIB_H
