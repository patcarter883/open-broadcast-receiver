// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_RECEIVE_RECEIVE_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_RECEIVE_RECEIVE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "RISTNet.h"

#include "lib/lib.h"

// rist_receive wraps a RISTNetReceiver that LISTENS for the encoder's
// RISTNetSender (caller). Received MPEG-TS payloads are handed to a push
// callback (the restream appsrc); ~1 Hz it computes link quality + worst-case
// RTT and sends the 5-byte wan_telemetry struct back to the encoder over the
// RIST OOB channel (closing the encoder's remote_oob adaptive-bitrate loop).
// See docs/CONTRACT.md §8 and DECISIONS.md §4/§6.
class rist_receive
{
public:
  using push_fn = std::function<int(const uint8_t*, std::size_t)>;
  // Fired (on a librist worker thread) when the encoder RE-connects after a
  // prior connection — i.e. NOT the very first connect. The handler MUST NOT
  // block the calling thread (it runs on the librist worker); it should only
  // signal a supervisor to re-arm codec detection / restart the restream
  // pipeline. See main.cpp.
  using reconnect_fn = std::function<void()>;

  explicit rist_receive(std::function<void(const std::string&)> log);
  ~rist_receive();
  rist_receive(const rist_receive&) = delete;
  auto operator=(const rist_receive&) -> rist_receive& = delete;
  rist_receive(rist_receive&&) = delete;
  auto operator=(rist_receive&&) -> rist_receive& = delete;

  // Build the listener, wire callbacks (ADVANCED profile) and start receiving.
  // `state` is borrowed for telemetry mirroring. Returns false + err on failure.
  auto start(const receiver_config& cfg,
             receiver_state* state,
             push_fn push,
             std::string& err) -> bool;

  auto stop() -> void;

  // Register the reconnect hook. Set before start(); fired from the librist
  // worker thread on a re-connect (not the first connect). Must not block.
  auto set_on_reconnect(reconnect_fn on_reconnect) -> void
  {
    m_on_reconnect = std::move(on_reconnect);
  }

private:
  auto log(const std::string& msg) const -> void;

  std::function<void(const std::string&)> m_log_func;
  std::unique_ptr<RISTNetReceiver> m_receiver;
  push_fn m_push;
  reconnect_fn m_on_reconnect;
  receiver_state* m_state = nullptr;  // borrowed

  // The connected encoder peer, captured from networkDataCallback and cleared
  // in clientDisconnectedCallback. Accessed from librist callback threads.
  std::atomic<rist_peer*> m_peer {nullptr};
  std::atomic_bool m_started {false};
  // True once the encoder has connected at least once. Used to distinguish a
  // first connect from a RE-connect so on_reconnect only fires on the latter.
  // Survives across listener restarts (only reset by an explicit listener teardown).
  std::atomic_bool m_was_connected {false};
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_RECEIVE_RECEIVE_H
