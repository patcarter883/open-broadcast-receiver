// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_RECORDER_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_RECORDER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "fanout/ring.h"

// recorder — the verbatim-TS recording consumer (TRANSPORT_PROFILE §3.7): a
// dedicated thread copying its ring cursor through a 1 MiB userspace buffer
// into <record-dir>/<session_id>.ts via plain write(2), fdatasync every 5 s.
// No GStreamer involved. A stalling disk makes THIS consumer fall behind and
// take ring drops (dropped_bytes accounted like any other consumer) — a dying
// disk degrades the recording, never the stream.
class recorder
{
public:
  using log_fn = std::function<void(const std::string&)>;

  recorder(std::string record_dir,
           std::string session_id,
           ts_ring& ring,
           log_fn log);
  ~recorder();
  recorder(const recorder&) = delete;
  auto operator=(const recorder&) -> recorder& = delete;
  recorder(recorder&&) = delete;
  auto operator=(recorder&&) -> recorder& = delete;

  // Open the file + attach a cursor + launch the writer thread. Returns false
  // (with a log line) if the file cannot be created; the session proceeds
  // without recording — recording failure is never session-fatal.
  auto start() -> bool;

  // Drain what is immediately available, flush, close. Idempotent.
  auto stop() -> void;

  [[nodiscard]] auto active() const -> bool
  {
    return m_active.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto bytes_written() const -> uint64_t
  {
    return m_bytes.load(std::memory_order_relaxed);
  }
  [[nodiscard]] auto dropped_bytes() const -> uint64_t
  {
    return (m_consumer >= 0) ? m_ring.dropped_bytes(m_consumer) : 0;
  }
  [[nodiscard]] auto path() const -> const std::string& { return m_path; }

private:
  auto worker() -> void;
  auto log(const std::string& msg) const -> void;

  std::string m_path;
  ts_ring& m_ring;
  log_fn m_log;

  int m_fd = -1;
  int m_consumer = -1;
  std::thread m_thread;
  std::atomic_bool m_stopping {false};
  std::atomic_bool m_active {false};
  std::atomic<uint64_t> m_bytes {0};
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_RECORDER_H
