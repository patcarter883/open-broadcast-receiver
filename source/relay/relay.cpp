// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "relay/relay.h"

#include "lib/lib.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include <pthread.h>
#include <sys/stat.h>

#include <librist/librist.h>

// HAVE_SRP_SUPPORT is set by librist's generated config; the SRP entry points
// (and their header) only exist when the library was built with nettle/mbedtls.
#include <librist/librist_srp.h>

#include "relay/relay_srp.h"

namespace relay
{
namespace
{
constexpr int k_log_interval_ms = 1000;
constexpr std::uint32_t k_flow_id = 0;
// librist copies the secret into a fixed RIST_MAX_STRING_SHORT field; anything
// longer would be silently truncated into a key nobody can match.
constexpr std::size_t k_max_secret_len = RIST_MAX_STRING_SHORT - 1;

auto now_ms() -> std::int64_t
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// librist log callback: everything goes to stderr so stdout stays a clean,
// line-parseable stats stream for the agent's log capture.
auto librist_log_cb(void* /*arg*/,
                    enum rist_log_level /*level*/,
                    const char* msg) -> int
{
  librist_log_write(msg);
  return 0;
}

// Copies the verifier/salt librist asked for into librist-owned heap memory.
auto fill_verifier(const srp_credential& cred,
                   librist_verifier_lookup_data_t* lookup_data) -> void
{
  const auto dup_bytes = [](const std::vector<std::uint8_t>& src,
                            std::uint8_t** dst) -> bool
  {
    auto* buf = static_cast<std::uint8_t*>(std::malloc(src.size()));
    if (buf == nullptr) {
      return false;
    }
    std::memcpy(buf, src.data(), src.size());
    *dst = buf;
    return true;
  };
  if (lookup_data == nullptr) {
    return;
  }
  if (!dup_bytes(cred.verifier, &lookup_data->verifier)) {
    return;
  }
  lookup_data->verifier_len = cred.verifier.size();
  if (!dup_bytes(cred.salt, &lookup_data->salt)) {
    std::free(lookup_data->verifier);
    lookup_data->verifier = nullptr;
    lookup_data->verifier_len = 0;
    return;
  }
  lookup_data->salt_len = cred.salt.size();
  lookup_data->default_ng = true;  // 2048-bit modulus + generator 2
  lookup_data->n_modulus_ascii = nullptr;
  lookup_data->generator_ascii = nullptr;
}

// SRP user lookup for the sender leg. librist calls this during the handshake
// with the username the peer presented; the file is re-read per lookup so a
// rotated credential takes effect without restarting the relay (ristsrppasswd
// rewriting the mounted file is the rotation path).
auto srp_lookup(char* username,
                librist_verifier_lookup_data_t* lookup_data,
                int* hashversion,
                std::uint64_t* generation,
                void* user_data) -> void
{
  const auto* srp_path = static_cast<const std::string*>(user_data);
  if (srp_path == nullptr || username == nullptr) {
    return;
  }
  // `generation` is non-null on the polling call, where librist only wants the
  // file's fingerprint; it is null when verifier data is being requested.
  if (generation != nullptr) {
    struct stat st {};
    if (::stat(srp_path->c_str(), &st) == 0) {
      *generation = (static_cast<std::uint64_t>(st.st_mtim.tv_sec) << 32)
          | static_cast<std::uint64_t>(st.st_mtim.tv_nsec & 0xFFFFFFFF);
    }
  }
  if (lookup_data == nullptr || hashversion == nullptr) {
    return;
  }
  const std::vector<srp_credential> creds = load_srp_file(*srp_path);
  // IN: the newest version librist supports (1 on EAP v3). OUT: what we served
  // — librist sets eapversion3 from it, so reporting the file's real version is
  // mandatory, not cosmetic.
  const srp_credential* cred =
      find_srp_credential(creds, std::string_view(username), *hashversion);
  if (cred == nullptr) {
    return;
  }
  *hashversion = cred->hashversion;
  fill_verifier(*cred, lookup_data);
}

struct relay_counters
{
  std::atomic<std::uint64_t> in_packets {0};
  std::atomic<std::uint64_t> in_bytes {0};
  std::atomic<std::uint64_t> out_packets {0};
  std::atomic<std::uint64_t> out_bytes {0};
  std::atomic<std::uint64_t> write_errors {0};
  std::atomic<int> peers {0};
};

struct relay_state
{
  struct rist_ctx* receiver_ctx = nullptr;
  struct rist_ctx* sender_ctx = nullptr;
  relay_counters counters;
  std::string srp_path;  // owned because the lookup callback borrows it
  log_fn log = nullptr;
};

auto say(const relay_state& state, const std::string& msg) -> void
{
  if (state.log != nullptr) {
    state.log(msg);
    return;
  }
  std::fputs(msg.c_str(), stdout);
  std::fflush(stdout);
}

// Receiver data callback. Runs on librist's per-flow output thread and must not
// stall; the sender write below is a queue push.
auto on_receive(void* arg, struct rist_data_block* block) -> int
{
  auto* state = static_cast<relay_state*>(arg);
  if (state == nullptr || block == nullptr || state->sender_ctx == nullptr) {
    return 0;
  }

  state->counters.in_packets.fetch_add(1, std::memory_order_relaxed);
  state->counters.in_bytes.fetch_add(block->payload_len,
                                     std::memory_order_relaxed);

  // Forward the block as it arrived, preserving the venue sender's sequence
  // number (USE_SEQ) and flow id. This is exactly what rist2rist does; a plain
  // rist_sender_write would restamp the sequence and the far receiver could no
  // longer tell a relay gap from a venue gap.
  struct rist_data_block out {};
  out.payload = block->payload;
  out.payload_len = block->payload_len;
  out.ts_ntp = block->ts_ntp;
  out.virt_src_port = block->virt_src_port;
  out.virt_dst_port = block->virt_dst_port;
  out.flow_id = block->flow_id;
  out.seq = block->seq;
  out.flags = RIST_DATA_FLAGS_USE_SEQ;

  const int ret = rist_sender_data_write(state->sender_ctx, &out);
  if (ret < 0) {
    state->counters.write_errors.fetch_add(1, std::memory_order_relaxed);
  } else {
    state->counters.out_packets.fetch_add(1, std::memory_order_relaxed);
    state->counters.out_bytes.fetch_add(block->payload_len,
                                        std::memory_order_relaxed);
  }
  // The block (and its payload) is librist's until we free it; the sender has
  // copied what it needs by the time write returns.
  return 0;
}

auto on_auth_connect(void* arg,
                     const char* connecting_ip,
                     std::uint16_t /*port*/,
                     const char* /*local_ip*/,
                     std::uint16_t /*local_port*/,
                     struct rist_peer* /*peer*/) -> int
{
  if (auto* state = static_cast<relay_state*>(arg); state != nullptr) {
    state->counters.peers.fetch_add(1, std::memory_order_relaxed);
    if (state->log != nullptr) {
      state->log("relay: venue sender connected from "
                 + std::string(connecting_ip != nullptr ? connecting_ip : "?")
                 + "\n");
    }
  }
  return 0;  // authorize: the in leg authenticates by SRP/PSK, not by list
}

auto on_auth_disconnect(void* arg, struct rist_peer* /*peer*/) -> int
{
  if (auto* state = static_cast<relay_state*>(arg); state != nullptr) {
    state->counters.peers.fetch_sub(1, std::memory_order_relaxed);
  }
  return 0;
}
}  // namespace

auto validate_relay_config(const relay_config& cfg, std::string& err) -> bool
{
  if (cfg.in_url.empty() || cfg.out_url.empty()) {
    err = "relay mode needs both --relay-in and --relay-out";
    return false;
  }
  // Both legs bind. A leg without '@' would make this end the CALLER on that
  // leg; for the in-leg that means the relay dials the venue (which is behind
  // the venue's NAT), and for the out-leg it means every relay start depends on
  // the node's receiver already listening. "@" on both is what makes the
  // handshake direction-independent, so it is required rather than inferred.
  if (cfg.in_url.find('@') == std::string::npos
      || cfg.out_url.find('@') == std::string::npos)
  {
    err = "relay legs must both bind ('@' in the URL): --relay-in and "
          "--relay-out";
    return false;
  }
  if (!cfg.in_url.starts_with("rist://") && !cfg.in_url.starts_with("rist6://")) {
    err = "--relay-in must be a rist:// or rist6:// URL";
    return false;
  }
  if (!cfg.out_url.starts_with("rist://") && !cfg.out_url.starts_with("rist6://")) {
    err = "--relay-out must be a rist:// or rist6:// URL";
    return false;
  }
  if (cfg.aes != 128 && cfg.aes != 256) {
    err = "--relay-aes must be 128 or 256";
    return false;
  }
  if (cfg.secret.size() > k_max_secret_len) {
    err = "--relay-secret is longer than librist accepts";
    return false;
  }
  if (!cfg.srp_path.empty()) {
    // Fail here, before any socket exists: a bound sender leg that will refuse
    // every peer is a silent black hole for a customer's stream.
    const std::vector<srp_credential> creds = load_srp_file(cfg.srp_path);
    if (creds.empty()) {
      err = "SRP file is missing, unreadable, or has no usable entry: "
          + cfg.srp_path;
      return false;
    }
  }
  return true;
}

auto run_relay(const relay_config& cfg, log_fn log) -> int
{
  std::string err;
  if (!validate_relay_config(cfg, err)) {
    std::fprintf(stderr, "relay: %s\n", err.c_str());
    return 2;
  }

  relay_state state;
  state.log = log;
  state.srp_path = cfg.srp_path;

  struct rist_logging_settings* logging = nullptr;
  if (rist_logging_set(&logging, RIST_LOG_INFO, nullptr, nullptr, nullptr, nullptr)
      != 0 || logging == nullptr)
  {
    std::fprintf(stderr, "relay: could not initialise librist logging\n");
    return 1;
  }
  logging->log_cb = &librist_log_cb;

  // ---- in leg: a bound receiver -------------------------------------------
  if (rist_receiver_create(&state.receiver_ctx, RIST_PROFILE_ADVANCED, logging)
      != 0)
  {
    std::fprintf(stderr, "relay: rist_receiver_create failed\n");
    return 1;
  }
  rist_receiver_data_callback_set2(state.receiver_ctx, &on_receive, &state);
  rist_auth_handler_set(state.receiver_ctx, &on_auth_connect,
                        &on_auth_disconnect, &state);

  // librist seeds a config with its CURRENT defaults when handed a NULL
  // pointer, then applies the URL's parameters to it. A zero-initialised
  // `struct rist_peer_config {}` is NOT the defaults: it means bufmin=0,
  // bufmax=0 (retransmission effectively off), rttmin=rttmax=3 and
  // congestion_control=0, i.e. a silently degraded leg. Let the library seed it.
  struct rist_peer_config* in_peer_config = nullptr;
  if (rist_parse_address2(cfg.in_url.c_str(), &in_peer_config) != 0
      || in_peer_config == nullptr)
  {
    std::fprintf(stderr, "relay: cannot parse --relay-in: %s\n",
                 loggable_endpoint(cfg.in_url).c_str());
    return 1;
  }
  in_peer_config->profile = RIST_PROFILE_ADVANCED;
  in_peer_config->profile_set = 1;
  if (!cfg.secret.empty()) {
    // BOTH legs are public hops, so both decrypt: without this the in leg
    // receives an encrypted venue stream and drops every packet with
    // "Receiving encrypted data, but configured without keysize" while the
    // session reports itself healthy and carries nothing.
    in_peer_config->key_size = cfg.aes;
    std::strncpy(in_peer_config->secret, cfg.secret.c_str(), k_max_secret_len);
  }
  struct rist_peer* in_peer = nullptr;
  const int in_rc =
      rist_peer_create(state.receiver_ctx, &in_peer, in_peer_config);
  std::free(in_peer_config);
  if (in_rc != 0) {
    std::fprintf(stderr, "relay: in leg failed to bind: %s\n",
                 loggable_endpoint(cfg.in_url).c_str());
    return 1;
  }
  if (!cfg.srp_path.empty()) {
    // BOTH legs authenticate, not just the out leg. The venue peer is the
    // untrusted one — it is the customer's encoder on the public internet — so
    // leaving the in leg without a verifier makes the relay accept the stream
    // anonymously while refusing (or silently ignoring) its SRP handshake:
    // measured live, every connecting sender died with "EAP authentication
    // requested but credentials have not been configured!" and the relay
    // carried nothing while the session reported itself healthy.
    const int srp_rc = rist_enable_eap_srp_2(in_peer, nullptr, nullptr,
                                             &srp_lookup, &state.srp_path);
    if (srp_rc != 0) {
      std::fprintf(stderr,
                   "relay: in leg SRP enabling failed (%d) for %s\n", srp_rc,
                   cfg.srp_path.c_str());
      return 1;
    }
  }
  say(state,
      "relay: in leg URL listening mode: " + loggable_endpoint(cfg.in_url)
          + "\n");

  // ---- out leg: a bound (listening) sender --------------------------------
  if (rist_sender_create(&state.sender_ctx, RIST_PROFILE_ADVANCED, k_flow_id,
                         logging)
      != 0)
  {
    std::fprintf(stderr, "relay: rist_sender_create failed\n");
    return 1;
  }

  struct rist_peer_config* out_peer_config = nullptr;
  if (rist_parse_address2(cfg.out_url.c_str(), &out_peer_config) != 0
      || out_peer_config == nullptr)
  {
    std::fprintf(stderr, "relay: cannot parse --relay-out: %s\n",
                 loggable_endpoint(cfg.out_url).c_str());
    return 1;
  }
  out_peer_config->profile = RIST_PROFILE_ADVANCED;
  out_peer_config->profile_set = 1;
  if (!cfg.secret.empty()) {
    // The AES secret never enters the URL (it would be logged and visible in
    // `ps`); it rides the peer config into rist_peer_config.secret.
    out_peer_config->key_size = cfg.aes;
    std::strncpy(out_peer_config->secret, cfg.secret.c_str(), k_max_secret_len);
  }
  struct rist_peer* out_peer = nullptr;
  const int out_rc =
      rist_peer_create(state.sender_ctx, &out_peer, out_peer_config);
  std::free(out_peer_config);
  if (out_rc != 0) {
    std::fprintf(stderr, "relay: out leg failed to bind: %s\n",
                 loggable_endpoint(cfg.out_url).c_str());
    return 1;
  }
  if (!cfg.srp_path.empty()) {
    const int srp_rc = rist_enable_eap_srp_2(out_peer, nullptr, nullptr,
                                             &srp_lookup, &state.srp_path);
    if (srp_rc != 0) {
      std::fprintf(stderr,
                   "relay: out leg SRP enabling failed (%d) for %s\n", srp_rc,
                   cfg.srp_path.c_str());
      return 1;
    }
  }
  say(state, "relay: out leg URL listening mode: "
                 + loggable_endpoint(cfg.out_url)
                 + " (Initialized Sender Peer, listening mode)\n");

  if (rist_start(state.sender_ctx) != 0) {
    std::fprintf(stderr, "relay: rist_start(sender) failed\n");
    return 1;
  }
  if (rist_start(state.receiver_ctx) != 0) {
    std::fprintf(stderr, "relay: rist_start(receiver) failed\n");
    return 1;
  }

  // Both legs bound and started before a single packet is read: a relay that
  // half-bound must die loudly here, not look healthy from the venue's side.
  say(state, "relay: both legs bound; pumping\n");

  // ---- signal handling + 1 Hz counters ------------------------------------
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

  int exit_code = 0;
  std::int64_t next_report = now_ms() + k_log_interval_ms;
  for (;;) {
    const struct timespec wait {.tv_sec = 0, .tv_nsec = 200'000'000};
    const int signo = sigtimedwait(&sigs, nullptr, &wait);
    if (signo == SIGINT || signo == SIGTERM) {
      say(state, "relay: stopping on signal\n");
      break;
    }
    const std::int64_t now = now_ms();
    if (now >= next_report) {
      next_report = now + k_log_interval_ms;
      // Same shape as the receiver's stats line: plain key=value so the agent's
      // existing log capture needs no new parser.
      char line[256];
      std::snprintf(line, sizeof(line),
                    "relay stats: in_packets=%llu in_bytes=%llu "
                    "out_packets=%llu out_bytes=%llu peers=%d\n",
                    static_cast<unsigned long long>(
                        state.counters.in_packets.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        state.counters.in_bytes.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        state.counters.out_packets.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        state.counters.out_bytes.load(std::memory_order_relaxed)),
                    state.counters.peers.load(std::memory_order_relaxed));
      say(state, line);
    }
  }

  rist_destroy(state.receiver_ctx);
  rist_destroy(state.sender_ctx);
  return exit_code;
}

}  // namespace relay
