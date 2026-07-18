// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "receive/receive.h"

#include <chrono>
#include <string_view>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <arpa/inet.h>

#include "RISTNet.h"

namespace
{
// librist C log callback (headless: route to stderr). Signature matches
// rist_logging_settings::log_cb.
auto librist_log_cb(void* /*arg*/,
                    enum rist_log_level /*level*/,
                    const char* msg) -> int
{
  if (msg != nullptr) {
    std::fputs(msg, stderr);
  }
  return 0;
}

auto steady_ms() -> int64_t
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

rist_receive::rist_receive(std::function<void(const std::string&)> log)
    : m_log_func {std::move(log)}
{
}

rist_receive::~rist_receive()
{
  stop();
}

auto rist_receive::log(const std::string& msg) const -> void
{
  if (m_log_func) {
    m_log_func(msg);
  }
}

auto rist_receive::flow_stats() const -> rist_flow_stat
{
  std::lock_guard<std::mutex> guard(m_flow_mutex);
  return m_flow;
}

auto rist_receive::start(const receiver_config& cfg,
                          const runtime_options& opts,
                          receiver_state* state,
                          push_fn push,
                          std::string& err) -> bool
{
  m_state = state;
  m_push = std::move(push);
  m_peer.store(nullptr, std::memory_order_release);

  // (Re)create the receiver and wire callbacks. Recreated on the v4 retry
  // below — a failed initReceiver leaves the instance torn down.
  const auto wire_receiver = [this]() -> void
  {
  m_receiver = std::make_unique<RISTNetReceiver>();

  // --- callbacks (wired BEFORE initReceiver) ---

  m_receiver->validateConnectionCallback =
      [this](const std::string& addr, uint16_t port)
      -> std::shared_ptr<RISTNetReceiver::NetworkConnection>
  {
    // Bonded sessions legitimately present several peers on this one port
    // (one per WAN path), and cellular CGNAT rebinds source addresses
    // mid-stream — every connect is welcome and nothing is restarted.
    log("RIST peer connected from " + addr + ":" + std::to_string(port)
        + "\n");
    return std::make_shared<RISTNetReceiver::NetworkConnection>();
  };

  m_receiver->networkDataCallback =
      [this](const uint8_t* buf,
             std::size_t len,
             std::shared_ptr<RISTNetReceiver::NetworkConnection>& /*conn*/,
             rist_peer* peer_ptr,
             uint16_t /*connID*/) -> int
  {
    // Track the most recent data peer for OOB telemetry + media liveness for
    // the idle watchdog. The push target is the SPMC ring: it memcpys and
    // returns — this thread is never blocked by consumers (§3.4).
    m_peer.store(peer_ptr, std::memory_order_release);
    if (m_state != nullptr) {
      m_state->have_peer.store(true, std::memory_order_relaxed);
      m_state->rist_bytes.fetch_add(len, std::memory_order_relaxed);
      m_state->last_payload_ms.store(steady_ms(), std::memory_order_relaxed);
    }
    if (m_push) {
      return m_push(buf, len);
    }
    return 0;  // keep the connection
  };

  m_receiver->statisticsCallback = [this](const rist_stats& stats) -> void
  {
    if (stats.stats_type != RIST_STATS_RECEIVER_FLOW) {
      return;
    }
    const rist_stats_receiver_flow& flow = stats.stats.receiver_flow;

    double quality = flow.quality;
    if (quality < 0.0) {
      quality = 0.0;
    } else if (quality > 100.0) {
      quality = 100.0;
    }
    const auto link_quality = static_cast<uint8_t>(quality + 0.5);

    uint32_t worst_rtt = flow.rtt;
    if (flow.peers != nullptr) {
      for (uint32_t idx = 0; idx < flow.peer_count; ++idx) {
        if (flow.peers[idx].rtt > worst_rtt) {
          worst_rtt = static_cast<uint32_t>(flow.peers[idx].rtt);
        }
      }
    }

    if (m_state != nullptr) {
      m_state->link_quality.store(link_quality, std::memory_order_relaxed);
      m_state->worst_rtt.store(worst_rtt, std::memory_order_relaxed);
    }

    // Full snapshot for GET /stats (§1.4): everything this vendored librist's
    // receiver_flow exposes, including the per-peer counters (presence
    // verified at implementation — closes TRANSPORT_PROFILE §5.1).
    {
      std::lock_guard<std::mutex> guard(m_flow_mutex);
      m_flow.quality = quality;
      m_flow.rtt_ms = flow.rtt;
      m_flow.received = flow.received;
      m_flow.missing = flow.missing;
      m_flow.recovered = flow.recovered;
      m_flow.recovered_one_retry = flow.recovered_one_retry;
      m_flow.lost = flow.lost;
      m_flow.reordered = flow.reordered;
      m_flow.bandwidth_bps = flow.bandwidth;
      m_flow.retry_bandwidth_bps = flow.retry_bandwidth;
      m_flow.peers.clear();
      if (flow.peers != nullptr) {
        for (uint32_t idx = 0; idx < flow.peer_count; ++idx) {
          const auto& peer = flow.peers[idx];
          m_flow.peers.push_back(rist_peer_stat {
              .id = peer.peer_id,
              .rtt_ms = static_cast<uint32_t>(peer.rtt),
              .avg_rtt_ms = peer.avg_rtt,
              .received = peer.received_data,
              .received_bytes = peer.received_bytes,
              .bandwidth_bps = peer.bandwidth});
        }
      }
    }

    // Send the 5-byte wan_telemetry back to the encoder (best-effort). The
    // vendored sendOOBData is patched to NOT tear down the receiver on a
    // transient failure (DECISIONS.md §6).
    rist_peer* peer_ptr = m_peer.load(std::memory_order_acquire);
    if (peer_ptr != nullptr) {
      uint8_t pkt[sizeof(wan_telemetry)];
      pkt[0] = link_quality;
      const uint32_t net_order = htonl(worst_rtt);
      std::memcpy(&pkt[1], &net_order, sizeof(net_order));
      m_receiver->sendOOBData(peer_ptr, pkt, sizeof(pkt));
    }
  };

  m_receiver->clientDisconnectedCallback =
      [this](const std::shared_ptr<RISTNetReceiver::NetworkConnection>& /*conn*/,
             const rist_peer& /*peer*/) -> void
  {
    // A bonded session may have other live peers; only clear the OOB target
    // (the next data packet from any surviving peer re-captures it).
    m_peer.store(nullptr, std::memory_order_release);
    log("RIST peer disconnected.\n");
  };

  };  // wire_receiver

  // --- settings ---
  RISTNetReceiver::RISTNetReceiverSettings settings;
  settings.mProfile = RIST_PROFILE_ADVANCED;  // must match encoder
  settings.mLogLevel = RIST_LOG_INFO;
  if (settings.mLogSetting) {
    settings.mLogSetting->log_cb = &librist_log_cb;
  }
  settings.mPeerConfig.recovery_length_min = cfg.ingest.buffer_min;
  settings.mPeerConfig.recovery_length_max = cfg.ingest.buffer_max;
  settings.mPeerConfig.recovery_rtt_min = cfg.ingest.rtt_min;
  settings.mPeerConfig.recovery_rtt_max = cfg.ingest.rtt_max;
  settings.mPeerConfig.recovery_reorder_buffer = cfg.ingest.reorder_buffer;
  settings.mPeerConfig.recovery_maxbitrate = cfg.ingest.bandwidth;
  if (!opts.psk.empty()) {
    // Secrets never enter URLs: the PSK rides the settings struct into
    // rist_peer_config.secret. Hosted sessions always set it (BACKPLANE §1.1).
    settings.mPSK = opts.psk;
    settings.mPSKKeySize = opts.psk_aes;
  }

  const std::string url = build_listener_url(cfg.ingest);
  log("RIST listening on: " + url.substr(0, url.find('?')) + "\n");

  wire_receiver();
  std::vector<std::string> urls {url};
  if (!m_receiver->initReceiver(urls, settings)) {
    // The default @[::] dual-stack bind fails outright on IPv6-less hosts
    // (containers without ::, v4-only VPSes). Fall back to the v4 any
    // address once before giving up.
    constexpr std::string_view k_v6_any = "rist://@[::]:";
    if (url.starts_with(k_v6_any)) {
      const std::string v4_url =
          "rist://@0.0.0.0:" + url.substr(k_v6_any.size());
      log("IPv6 listen failed; retrying on "
          + v4_url.substr(0, v4_url.find('?')) + "\n");
      wire_receiver();  // fresh instance; a failed init tears down internals
      std::vector<std::string> v4_urls {v4_url};
      if (m_receiver->initReceiver(v4_urls, settings)) {
        m_started.store(true, std::memory_order_release);
        return true;
      }
    }
    err = "initReceiver failed (check listen URL / port availability)";
    m_receiver.reset();
    return false;
  }

  m_started.store(true, std::memory_order_release);
  return true;
}

auto rist_receive::stop() -> void
{
  if (m_receiver) {
    // destroyReceiver() runs rist_destroy(), which joins the librist worker /
    // stats threads — once it returns, no callback can fire. Do it FIRST, then
    // clear the std::function callbacks. We deliberately do NOT call
    // closeAllClientConnections() first: that path runs clientDisconnect (and
    // destroys peers) while the stats thread may still be calling sendOOBData
    // on the same peer. destroyReceiver() tears down peers race-free.
    m_receiver->destroyReceiver();
    m_receiver->networkDataCallback = nullptr;
    m_receiver->statisticsCallback = nullptr;
    m_receiver->validateConnectionCallback = nullptr;
    m_receiver->clientDisconnectedCallback = nullptr;
    m_receiver.reset();
  }
  m_peer.store(nullptr, std::memory_order_release);
  m_started.store(false, std::memory_order_release);
  if (m_state != nullptr) {
    m_state->have_peer.store(false, std::memory_order_relaxed);
    m_state->link_quality.store(0, std::memory_order_relaxed);
    m_state->worst_rtt.store(0, std::memory_order_relaxed);
  }
}
