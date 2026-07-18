// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "receive/receive.h"

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

auto rist_receive::start(const receiver_config& cfg,
                          receiver_state* state,
                          push_fn push,
                          std::string& err) -> bool
{
  m_state = state;
  m_push = std::move(push);
  m_peer.store(nullptr, std::memory_order_release);

  m_receiver = std::make_unique<RISTNetReceiver>();

  // --- callbacks (wired BEFORE initReceiver) ---

  m_receiver->validateConnectionCallback =
      [this](const std::string& addr, uint16_t port)
      -> std::shared_ptr<RISTNetReceiver::NetworkConnection>
  {
    // First connect vs RE-connect: if the encoder has connected before, the
    // incoming stream may carry a different codec, so re-run detection +
    // restart the restream pipeline. exchange() makes the first-connect
    // detection race-free against concurrent librist workers.
    const bool reconnect = m_was_connected.exchange(true, std::memory_order_acq_rel);
    if (reconnect) {
      log("Encoder reconnecting from " + addr + ":" + std::to_string(port)
          + "; re-arming detection\n");
      if (m_on_reconnect) {
        // Signal only — the handler defers the actual stop()/start() to a
        // supervisor thread; calling restream::stop() here would join the bus
        // thread from a librist worker and risk deadlock.
        m_on_reconnect();
      }
    } else {
      log("Encoder connecting from " + addr + ":" + std::to_string(port)
          + "\n");
    }
    return std::make_shared<RISTNetReceiver::NetworkConnection>();
  };

  m_receiver->networkDataCallback =
      [this](const uint8_t* buf,
             std::size_t len,
             std::shared_ptr<RISTNetReceiver::NetworkConnection>& /*conn*/,
             rist_peer* peer_ptr,
             uint16_t /*connID*/) -> int
  {
    // Capture the encoder peer for OOB telemetry. Single-encoder model.
    m_peer.store(peer_ptr, std::memory_order_release);
    if (m_state != nullptr) {
      m_state->have_peer.store(true, std::memory_order_relaxed);
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
    // Single encoder: clear the tracked peer so no telemetry is sent to a dead
    // pointer, and zero the telemetry mirror.
    m_peer.store(nullptr, std::memory_order_release);
    if (m_state != nullptr) {
      m_state->have_peer.store(false, std::memory_order_relaxed);
      m_state->link_quality.store(0, std::memory_order_relaxed);
      m_state->worst_rtt.store(0, std::memory_order_relaxed);
    }
    log("Encoder disconnected.\n");
  };

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

  std::vector<std::string> urls {build_listener_url(cfg.ingest)};
  log("RIST listening on: " + urls.front() + "\n");

  if (!m_receiver->initReceiver(urls, settings)) {
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
  m_was_connected.store(false, std::memory_order_release);
  if (m_state != nullptr) {
    m_state->have_peer.store(false, std::memory_order_relaxed);
  }
}
