// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_LIB_LIB_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_LIB_LIB_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations of the runtime components owned by app_context. Their
// full definitions live in their own modules; app_context is only ever
// destroyed in a translation unit (main.cpp) where the complete types are
// visible.
class rist_receive;
class control_server;
class ts_ring;
class output;
class recorder;

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

// Output protocols for the copy-only fan-out (CONTRACT §3, schema_version 2).
// The integer order is wire contract — do not renumber.
enum class output_proto : std::uint8_t
{
  rtmp,
  rtmps,
  srt,
  rist
};

// Opt-in transcode tier (schema_version 3): an output with
// target == none is copy-only (the default tier). A non-none target decodes
// the arriving elementary stream and re-encodes it for that output only; it
// does not relax the server-wide copy-only default. Integer order is wire
// contract — none must stay 0.
enum class transcode_target : std::uint8_t
{
  none,
  h264,
  h265
};

struct transcode_config
{
  transcode_target target = transcode_target::none;
  int bitrate_kbps = 0;  // 0 = encoder default
  int gop = 0;           // 0 = encoder default
  auto operator==(const transcode_config&) const -> bool = default;
};

// ---------------------------------------------------------------------------
// RIST OOB telemetry back-channel (receiver -> encoder). Byte-for-byte
// identical to open-broadcast-encoder/source/lib/lib.h:53-59. The encoder
// rejects any OOB payload whose size != sizeof(wan_telemetry). FROZEN: this
// struct is an ABR control signal, never a monitoring channel — see
// BACKPLANE.md §0/§13.4 (product repo).
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

// ---------------------------------------------------------------------------
// Configuration (schema_version 3 — TRANSPORT_PROFILE §1.1; v3 adds the
// optional outputs[].transcode object)
// ---------------------------------------------------------------------------

inline constexpr int k_schema_version = 3;
inline constexpr std::size_t k_max_outputs = 8;

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

// One fan-out destination. `url` and `key_or_streamid` are secrets-adjacent:
// the key is never echoed or logged, and the URL is validated + redacted per
// CONTRACT §2/§9. The pinned_* fields are runtime state produced by egress
// validation (M1.2): the vetted resolved IP that the pipeline MUST connect to
// (DNS-rebinding resistance — never re-resolve at connect/reconnect). They are
// deliberately excluded from equality so /start idempotency compares the wire
// config, not DNS weather.
struct output_config
{
  std::string id;
  output_proto type = output_proto::rtmp;
  std::string url;
  std::string key_or_streamid;

  std::string host;       // parsed authority host (kept for TLS SNI / tcUrl)
  int port = 0;           // parsed or scheme-default port
  std::string path;       // URL path (RTMP app), no query
  std::string pinned_ip;  // vetted resolved IP literal (connect target)

  // Opt-in per-output transcode (schema_version 3). Part of the wire config,
  // so it participates in equality; the runtime parsed/pinned fields above
  // stay deliberately excluded.
  transcode_config transcode;

  auto operator==(const output_config& other) const -> bool
  {
    return id == other.id && type == other.type && url == other.url
        && key_or_streamid == other.key_or_streamid
        && transcode == other.transcode;
  }
};

struct receiver_config
{
  int schema_version = k_schema_version;
  std::string session_id;
  ingest_config ingest;
  codec in_codec = codec::h264;  // the codec arriving over RIST (source.codec hint)
  std::vector<output_config> outputs;
  auto operator==(const receiver_config&) const -> bool = default;
};

// Operator-side runtime options that never appear on the wire (CLI flags).
struct runtime_options
{
  std::string psk;          // --psk <hex>; empty = no encryption (self-host)
  int psk_aes = 256;        // --psk-aes 128|256
  std::string record_dir;   // --record-dir; empty = no recording
  int idle_timeout_s = 0;   // --idle-timeout; 0 = off
};

// ---------------------------------------------------------------------------
// Egress policy (M1.2 / REVIEW C2) — SSRF & rebinding resistance
// ---------------------------------------------------------------------------

struct egress_policy
{
  // --egress-allow-private: self-host opt-out permitting RFC1918/ULA/CGNAT
  // destinations (LAN restreaming). Loopback, link-local (incl. the cloud
  // metadata range) and multicast stay forbidden even with the opt-out.
  bool allow_private = false;
  // --egress-deny <cidr,...>: unconditional deny-list, applied after the
  // class checks (the hosted agent passes the node subnet + metadata ranges).
  std::vector<std::string> deny_cidrs;
};

// Resolver injection point so validation is unit-testable with a rebinding
// stub. Fills `ips` with numeric literals (IPv4 dotted / IPv6 hex). The
// default implementation is getaddrinfo.
using resolve_fn =
    std::function<bool(const std::string& host, std::vector<std::string>& ips)>;

auto default_resolver() -> resolve_fn;

// Classify an IP literal against the policy. Returns nullptr when the address
// is an acceptable egress target, else a short reason ("loopback",
// "link_local", "private", "multicast", "denylist", "unparsable", ...).
auto forbidden_reason(const std::string& ip_literal,
                      const egress_policy& policy) -> const char*;

// ---------------------------------------------------------------------------
// Runtime state (shared across the control, receive and fanout threads)
// ---------------------------------------------------------------------------

struct receiver_state
{
  std::atomic_bool is_running {false};

  // Guards cfg, session_id, started_at, last_bus_error.
  std::mutex mutex;
  receiver_config cfg;
  std::string session_id;
  std::chrono::steady_clock::time_point started_at;
  std::string last_bus_error;

  // Telemetry mirror (also sent over RIST OOB). Lock-free for /status reads.
  std::atomic<int> link_quality {0};
  std::atomic<uint32_t> worst_rtt {0};
  std::atomic_bool have_peer {false};

  // Media liveness for the --idle-timeout watchdog: absolute payload byte
  // count and the steady-clock time of the last RIST payload.
  std::atomic<uint64_t> rist_bytes {0};
  std::atomic<int64_t> last_payload_ms {0};  // steady_clock ms; 0 = never
};

struct app_context
{
  receiver_state state;
  std::unique_ptr<rist_receive> receive;
  std::unique_ptr<control_server> control;
  std::string auth_token;  // empty => dev/no-auth mode
};

// ---------------------------------------------------------------------------
// Stats snapshots (GET /status outputs[] + GET /stats — TRANSPORT_PROFILE
// §1.3/§1.4). Built from atomics by main's snapshot callback; the /stats
// handler MUST NOT take pipeline locks or block the distribution path.
// ---------------------------------------------------------------------------

// Per-peer view (vendored librist rist_stats_receiver_peer — verified present
// at implementation, closing TRANSPORT_PROFILE §5.1): each link in a bond
// arrives as a separate peer, so this is the per-WAN-path panel view.
struct rist_peer_stat
{
  uint32_t id = 0;            // librist internal peer id
  uint32_t rtt_ms = 0;
  double avg_rtt_ms = 0.0;
  uint64_t received = 0;      // data packets from this peer
  uint64_t received_bytes = 0;
  uint64_t bandwidth_bps = 0;
};

struct rist_flow_stat
{
  double quality = 0.0;
  uint32_t rtt_ms = 0;
  uint64_t received = 0;
  uint64_t missing = 0;
  uint64_t recovered = 0;
  uint64_t recovered_one_retry = 0;
  uint64_t lost = 0;
  uint64_t reordered = 0;
  uint64_t bandwidth_bps = 0;
  uint64_t retry_bandwidth_bps = 0;
  std::vector<rist_peer_stat> peers;
};

struct output_stat
{
  std::string id;
  std::string type;
  std::string state;
  transcode_target transcode = transcode_target::none;  // none = copy-only
  int64_t connected_s = 0;
  uint64_t reconnects = 0;
  uint64_t bytes_sent = 0;
  uint64_t dropped_bytes = 0;
  bool audio_dropped = false;
  std::string last_error;  // empty = none; never contains URLs/keys
};

struct session_stats
{
  uint64_t ring_size_bytes = 0;
  bool recording_active = false;
  uint64_t recording_bytes = 0;
  uint64_t recording_dropped = 0;
  uint64_t rist_bytes_total = 0;  // ingest byte counter (agent derives rate)
  rist_flow_stat rist;
  std::vector<output_stat> outputs;
};

// ---------------------------------------------------------------------------
// Enum <-> string helpers (docs/CONTRACT.md §3)
// ---------------------------------------------------------------------------

auto to_string(codec cod) noexcept -> const char*;
auto to_string(output_proto proto) noexcept -> const char*;
auto to_string(transcode_target target) noexcept -> const char*;

auto parse_codec(std::string_view str, codec& out) noexcept -> bool;
auto parse_output_proto(std::string_view str, output_proto& out) noexcept
    -> bool;
// Parse a transcode target codec ("h264"|"h265"). "none" is NOT accepted:
// copy-only is expressed by omitting the transcode object, so a present
// transcode.codec must name a real target codec.
auto parse_transcode_target(std::string_view str,
                            transcode_target& out) noexcept -> bool;

// Build the RIST listener URL the receiver hands to initReceiver. Mirrors the
// encoder's recovery params and appends timing-mode=0 (SOURCE — every RIST hop
// runs SOURCE; ARRIVAL SIGABRTs under the double hop, see CONTRACT §4). The
// base (scheme + "@host:port") is taken from ingest.rist_listen up to any '?'.
// NOTE: it does NOT append a profile= URL param — librist's URL parser rejects
// it; the ADVANCED profile is set via RISTNetReceiverSettings.mProfile
// (see receive.cpp). The PSK never enters this URL (secrets never enter URLs);
// it travels via RISTNetReceiverSettings.mPSK.
auto build_listener_url(const ingest_config& ingest) -> std::string;

// Parse the numeric port from a "rist://@[::]:PORT" / "rist://@host:PORT"
// listen URL. Returns -1 if it cannot be determined.
auto listen_port_from_url(const std::string& rist_listen) noexcept -> int;

// Parse "scheme://host:port[/path][?...]" into host + port (handles bracketed
// IPv6). Returns false if host/port cannot be determined.
auto parse_authority(const std::string& url,
                     std::string& host,
                     int& port) noexcept -> bool;

// ---------------------------------------------------------------------------
// Validation (pure: no GStreamer, no JSON). Field-shape/type checks live in the
// JSON parser (control.cpp); these are the cross-field/value rules of
// CONTRACT §4 / TRANSPORT_PROFILE §1.1–1.2 that do not need the GStreamer
// registry.
// ---------------------------------------------------------------------------

struct validation_result
{
  bool ok = true;
  std::string error_code;  // e.g. "out_of_range", "bad_url", "forbidden_destination"
  std::string field;       // e.g. "ingest.bandwidth", "outputs[1].url"
  std::string message;
};

// Structural validation: ingest bounds, outputs count/id uniqueness,
// scheme/type match, URL parse + pipeline safety, rtmp_codec_unsupported.
// Fills outputs[].host/port/path as a side effect (parse once).
auto validate_config(receiver_config& cfg) -> validation_result;

// Egress validation (M1.2): resolve every output host through `resolve`,
// reject forbidden destinations per `policy`, and PIN the vetted IP into
// outputs[].pinned_ip. Error code "forbidden_destination" names the output ID
// only (never the resolved IP — don't leak topology). Call after
// validate_config; separated so unit tests can inject a rebinding resolver.
auto validate_and_pin_outputs(receiver_config& cfg,
                              const egress_policy& policy,
                              const resolve_fn& resolve) -> validation_result;

// True if `str` is safe to interpolate into a single-quoted gst_parse_launch
// property value (no quote-escape / control characters).
auto is_pipeline_safe(std::string_view str) noexcept -> bool;

// ---------------------------------------------------------------------------
// Element availability (TRANSPORT_PROFILE §1.2 element_unavailable)
// ---------------------------------------------------------------------------

// One element requirement. ANY listed factory name satisfies it — a transcode
// chain lists a VAAPI and a software alternative, so presence must be
// "any of". `transcode` marks the requirement as belonging to the opt-in
// transcode chain (so /start reports transcode_unavailable, not
// element_unavailable, when it is the one missing).
struct element_requirement
{
  std::vector<std::string> names;
  bool transcode = false;
  auto operator==(const element_requirement&) const -> bool = default;
};

// Elements one output template needs: the base chain for `proto`, plus the
// decode -> convert -> encode chain when `transcode.target != none` (the
// decoder is chosen from the source `in_codec`).
auto required_elements(output_proto proto,
                       const transcode_config& transcode,
                       codec in_codec) -> std::vector<element_requirement>;

// Registry probe, injected for testability: the real implementation is
// gst_element_factory_find; unit tests pass a stub.
using element_present_fn = std::function<bool(const char*)>;

struct missing_requirement
{
  std::string element;  // first name of the unsatisfied requirement
  bool transcode = false;
  auto operator==(const missing_requirement&) const -> bool = default;
};

// First requirement not satisfied by `present`, or nullopt when all are met.
auto first_missing_requirement(const std::vector<element_requirement>& reqs,
                               const element_present_fn& present)
    -> std::optional<missing_requirement>;

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_LIB_LIB_H
