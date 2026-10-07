// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "fanout/recorder.h"

#include <cerrno>
#include <chrono>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace
{
constexpr std::size_t k_write_buffer = 1024 * 1024;  // 1 MiB
constexpr std::chrono::milliseconds k_read_timeout {100};
constexpr std::chrono::seconds k_sync_interval {5};
constexpr mode_t k_file_mode = 0640;
}  // namespace

recorder::recorder(std::string record_dir,
                   std::string session_id,
                   ts_ring& ring,
                   log_fn log)
    : m_path {std::move(record_dir)}
    , m_ring {ring}
    , m_log {std::move(log)}
{
  if (!m_path.empty() && m_path.back() != '/') {
    m_path += '/';
  }
  m_path += session_id + ".ts";
}

recorder::~recorder()
{
  stop();
}

auto recorder::log(const std::string& msg) const -> void
{
  if (m_log) {
    m_log("[recorder] " + msg);
  }
}

auto recorder::start() -> bool
{
  m_fd = ::open(m_path.c_str(),
                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                k_file_mode);
  if (m_fd < 0) {
    log("cannot open " + m_path + " — session continues without recording\n");
    return false;
  }
  m_consumer = m_ring.add_consumer();
  if (m_consumer < 0) {
    ::close(m_fd);
    m_fd = -1;
    log("no ring slot — session continues without recording\n");
    return false;
  }
  m_stopping.store(false, std::memory_order_release);
  m_active.store(true, std::memory_order_release);
  m_thread = std::thread([this]() -> void { worker(); });
  log("recording to " + m_path + "\n");
  return true;
}

auto recorder::worker() -> void
{
  std::vector<uint8_t> buf(k_write_buffer);
  auto last_sync = std::chrono::steady_clock::now();

  const auto flush = [this](const uint8_t* data, std::size_t len) -> bool
  {
    std::size_t done = 0;
    while (done < len) {
      const ssize_t wrote = ::write(m_fd, data + done, len - done);
      if (wrote < 0) {
        if (errno == EINTR) {
          continue;
        }
        // Disk error: the recording degrades (consumer stalls → ring drops
        // account it); the stream is untouched. §3.7.
        log("write failed (errno=" + std::to_string(errno)
            + "); recording halted\n");
        return false;
      }
      done += static_cast<std::size_t>(wrote);
      m_bytes.fetch_add(static_cast<uint64_t>(wrote),
                        std::memory_order_relaxed);
    }
    return true;
  };

  while (!m_stopping.load(std::memory_order_acquire)) {
    const std::size_t got =
        m_ring.read(m_consumer, buf.data(), buf.size(), k_read_timeout);
    if (got > 0 && !flush(buf.data(), got)) {
      m_active.store(false, std::memory_order_release);
      return;  // fd left open for stop() to close
    }
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - last_sync >= k_sync_interval) {
      ::fdatasync(m_fd);
      last_sync = now_time;
    }
  }

  // Final drain of whatever is immediately available, then durable close.
  for (;;) {
    const std::size_t got = m_ring.read(
        m_consumer, buf.data(), buf.size(), std::chrono::milliseconds(0));
    if (got == 0 || !flush(buf.data(), got)) {
      break;
    }
  }
  ::fdatasync(m_fd);
}

auto recorder::stop() -> void
{
  m_stopping.store(true, std::memory_order_release);
  // The consumer slot and the fd are released BEFORE the join: a writer parked in
  // the frame ring or on the file is freed by them, and does not observe
  // m_stopping on its own.
  if (m_consumer >= 0) {
    m_ring.remove_consumer(m_consumer);
    m_consumer = -1;
  }
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
  if (m_thread.joinable()) {
    m_thread.join();
  }
  m_active.store(false, std::memory_order_release);
}
