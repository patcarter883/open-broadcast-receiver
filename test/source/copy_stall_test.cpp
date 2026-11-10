// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// copy_stall_test -- the default (copy) tier against a sink that stops
// draining.
//
// The defect this exists for: a copy output pushed into its appsrc with
// block=false and no level gate, so a destination that stopped reading grew the
// queue at the ingest rate (10 Mbps ≈ 4.5 GB/h) with no error, no rebuild and
// nothing in /stats to see. ingest never noticed, because the ring behind the
// cursor was the only dropper and it was never reached.
//
// The fixture is real, not synthetic bytes: an H.264-in-MPEG-TS clip generated
// in-process (videotestsrc -> x264enc -> mpegtsmux) and pumped into a ts_ring at
// a paced 10 Mbps, exactly as RIST ingest would. Two copy outputs consume it:
//
//   A -> rtmp://127.0.0.1:<proxy>/live/stall   (proxy forwards 3 s, then stops
//                                              reading: publish established,
//                                              then the sink stalls)
//   B -> rtmp://127.0.0.1:<mtx>/live/healthy   (MediaMTX, keeps publishing)
//
// Assertions, all three from the work order:
//   (a) A is rebuilt within the stall window: the sink is stalled after ~3 s of
//       publishing and the output must reconnect (and log the stall) inside the
//       5 s watchdog plus a rebuild, measured from the moment its appsrc first
//       pins at its cap.
//   (b) A's appsrc level never exceeds its cap by more than one push: the queue
//       is bounded, not the 10 Mbps the ring keeps offering.
//   (c) B keeps publishing across A's stall: its published-byte count grows and
//       it never reconnects -- one stalled destination must not cost the others.
//
// MediaMTX is the RTMP sink (already a test-harness dependency: see
// test/harness/receiver_e2e_local.sh). Without it the test exits 77 (skip)
// rather than pretending to have measured something.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "fanout/output.h"
#include "fanout/ring.h"
#include "lib/lib.h"

namespace
{
auto expect(bool cond, const std::string& what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
  }
}

auto now_ms() -> std::int64_t
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---- sockets ---------------------------------------------------------------

auto listen_loopback(int& port) -> int
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  expect(fd >= 0, "socket()");
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  expect(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0,
         "bind(loopback)");
  expect(::listen(fd, 4) == 0, "listen()");
  socklen_t len = sizeof addr;
  expect(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0,
         "getsockname()");
  port = ntohs(addr.sin_port);
  return fd;
}

auto connect_loopback(int port) -> int
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

auto wait_accepting(int port, std::chrono::seconds limit) -> bool
{
  const auto deadline =
      std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = connect_loopback(port);
    if (fd >= 0) {
      ::close(fd);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// A TCP relay that forwards the first `forward_for` and then stops reading from
// its client while leaving both sockets open. That is the stall under test: the
// publish exchange completed (so the output is "published"), and the receiver
// stops consuming without closing -- a wedged platform ingest, not an outage.
class stall_proxy
{
public:
  stall_proxy(std::string upstream_host, int upstream_port,
              std::chrono::seconds forward_for)
      : m_upstream_host {std::move(upstream_host)}
      , m_upstream_port {upstream_port}
      , m_forward_for {forward_for}
  {
    m_listen_fd = listen_loopback(m_port);
    m_thread = std::thread([this] { run(); });
  }

  ~stall_proxy()
  {
    m_stop.store(true);
    if (m_thread.joinable()) {
      m_thread.join();
    }
    ::close(m_listen_fd);
    if (m_client_fd >= 0) {
      ::close(m_client_fd);
    }
    if (m_upstream_fd >= 0) {
      ::close(m_upstream_fd);
    }
  }

  stall_proxy(const stall_proxy&) = delete;
  auto operator=(const stall_proxy&) -> stall_proxy& = delete;

  [[nodiscard]] auto port() const -> int { return m_port; }

private:
  auto run() -> void
  {
    const int accepted = ::accept(m_listen_fd, nullptr, nullptr);
    if (accepted < 0) {
      return;
    }
    m_client_fd = accepted;
    m_upstream_fd = connect_loopback(m_upstream_port);
    if (m_upstream_fd < 0) {
      return;
    }
    const auto started = std::chrono::steady_clock::now();
    bool forwarding = true;
    std::vector<char> buffer(64 * 1024);
    while (!m_stop.load()) {
      if (forwarding && std::chrono::steady_clock::now() - started
              > m_forward_for)
      {
        forwarding = false;
        m_stalled.store(true);
      }
      pollfd fds[2] {};
      int count = 0;
      int client_slot = -1;
      int upstream_slot = -1;
      if (forwarding) {
        client_slot = count;
        fds[count++] = pollfd {m_client_fd, POLLIN, 0};
      }
      // The return path stays open for the whole run: RTMP needs its reads,
      // and stopping both directions would look like a reset instead of a stall.
      upstream_slot = count;
      fds[count++] = pollfd {m_upstream_fd, POLLIN, 0};

      const int ready = ::poll(fds, static_cast<nfds_t>(count), 100);
      if (ready <= 0) {
        continue;
      }
      if (client_slot >= 0 && (fds[client_slot].revents & POLLIN)) {
        const ssize_t got = ::read(m_client_fd, buffer.data(), buffer.size());
        if (got > 0) {
          m_sent_to_upstream += static_cast<std::uint64_t>(got);
          ::send(m_upstream_fd, buffer.data(), static_cast<std::size_t>(got),
                 MSG_NOSIGNAL);
        }
      }
      if (upstream_slot >= 0 && (fds[upstream_slot].revents & POLLIN)) {
        const ssize_t got =
            ::read(m_upstream_fd, buffer.data(), buffer.size());
        if (got > 0) {
          ::send(m_client_fd, buffer.data(), static_cast<std::size_t>(got),
                 MSG_NOSIGNAL);
        }
      }
    }
  }

  std::string m_upstream_host;
  int m_upstream_port = 0;
  std::chrono::seconds m_forward_for {3};
  int m_listen_fd = -1;
  int m_port = 0;
  int m_client_fd = -1;
  int m_upstream_fd = -1;
  std::uint64_t m_sent_to_upstream = 0;
  std::atomic<bool> m_stop {false};
  std::atomic<bool> m_stalled {false};
  std::thread m_thread;
};

// ---- fixture ---------------------------------------------------------------

// Pull every buffer of a pipeline into one byte vector. The clip is generated,
// not committed: a binary TS whose NAL types nobody can inspect is a fixture
// that hides its own breakage.
auto pull_ts(std::string description) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> bytes;
  GError* gerr = nullptr;
  GstElement* pipeline = gst_parse_launch(description.c_str(), &gerr);
  if (pipeline == nullptr || gerr != nullptr) {
    if (gerr != nullptr) {
      std::fprintf(stderr, "fixture pipeline: %s\n", gerr->message);
      g_error_free(gerr);
    }
    return bytes;
  }
  GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  if (sink == nullptr) {
    gst_object_unref(pipeline);
    return bytes;
  }
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  for (;;) {
    GstSample* sample = gst_app_sink_try_pull_sample(
        GST_APP_SINK(sink), GST_SECOND * 30);
    if (sample == nullptr) {
      break;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buffer != nullptr && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      bytes.insert(bytes.end(), map.data, map.data + map.size);
      gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  return bytes;
}

// ---- MediaMTX --------------------------------------------------------------

struct mediamtx
{
  pid_t pid = -1;
  int port = 0;
  std::filesystem::path conf;
};

auto start_mediamtx(const std::filesystem::path& dir, int rtmp_port)
    -> mediamtx
{
  mediamtx mtx;
  mtx.port = rtmp_port;
  mtx.conf = dir / "mediamtx.yml";
  std::ofstream out {mtx.conf};
  out << std::format(
      "logLevel: error\nrtsp: no\nhls: no\nwebrtc: no\nsrt: no\nmoq: no\n"
      "rtmp: yes\nrtmpAddress: 127.0.0.1:{}\nrtmpEncryption: \"no\"\n"
      "api: no\npaths:\n  all_others:\n",
      rtmp_port);
  out.close();

  const pid_t pid = ::fork();
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
    }
    ::execlp("mediamtx", "mediamtx", mtx.conf.c_str(), nullptr);
    _exit(127);
  }
  mtx.pid = pid;
  return mtx;
}

auto stop_mediamtx(mediamtx& mtx) -> void
{
  if (mtx.pid > 0) {
    ::kill(mtx.pid, SIGTERM);
    ::waitpid(mtx.pid, nullptr, 0);
    mtx.pid = -1;
  }
}

// ---- main ------------------------------------------------------------------

constexpr double k_ingest_bps = 10.0e6;  // the work order's ~10 Mbps

auto feed(ts_ring& ring, const std::vector<std::uint8_t>& clip,
          std::chrono::steady_clock::time_point until) -> void
{
  // Paced at the bitrate under test, in RIST-sized chunks. A ring that is full
  // is not a problem: the producer never blocks by contract, and the drop is
  // what the outputs are meant to absorb.
  constexpr std::size_t chunk = 7 * 188;
  std::size_t offset = 0;
  const auto started = std::chrono::steady_clock::now();
  double sent = 0.0;
  while (std::chrono::steady_clock::now() < until) {
    ring.write(clip.data() + offset, chunk);
    offset += chunk;
    if (offset + chunk > clip.size()) {
      offset = 0;
    }
    sent += static_cast<double>(chunk);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
            .count();
    const double due = sent * 8.0 / k_ingest_bps;
    if (due > elapsed) {
      std::this_thread::sleep_for(
          std::chrono::duration<double>(due - elapsed));
    }
  }
}

auto appsrc_level(output& out, guint64& cap) -> guint64
{
  GstElement* pipeline = out.pipeline();
  if (pipeline == nullptr) {
    return 0;
  }
  guint64 level = 0;
  GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline), "osrc");
  if (src != nullptr) {
    level = gst_app_src_get_current_level_bytes(GST_APP_SRC(src));
    cap = gst_app_src_get_max_bytes(GST_APP_SRC(src));
    gst_object_unref(src);
  }
  gst_object_unref(pipeline);
  return level;
}

}  // namespace

auto main(int argc, char** argv) -> int
{
  gst_init(&argc, &argv);

  if (std::system("command -v mediamtx >/dev/null 2>&1") != 0) {
    std::fprintf(stderr,
                 "SKIP: mediamtx not installed; the stall test needs a real "
                 "RTMP sink\n");
    return 77;
  }
  if (gst_registry_check_feature_version(gst_registry_get(), "x264enc", 1, 0,
                                         0)
      != TRUE)
  {
    std::fprintf(stderr, "SKIP: x264enc not installed (fixture encoder)\n");
    return 77;
  }

  std::error_code ec;
  const auto dir = std::filesystem::temp_directory_path(ec)
      / std::format("obr-copy-stall-{}", ::getpid());
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  // 1080p30 at ~10 Mbps for 4 s: the clip loops, and its own rate is close to
  // the pace it is fed at, so the ring sees a continuous stream rather than a
  // burst of stale frames.
  const auto clip = pull_ts(
      "videotestsrc is-live=false num-buffers=120 pattern=ball ! "
      "video/x-raw,width=1280,height=720,framerate=30/1 ! "
      "x264enc bitrate=8000 speed-preset=ultrafast tune=zerolatency "
      "key-int-max=30 ! h264parse config-interval=-1 ! "
      "mpegtsmux alignment=7 ! appsink name=sink sync=false");
  expect(!clip.empty(), "generated an H.264-in-TS fixture");
  std::printf("fixture: %zu bytes of MPEG-TS\n", clip.size());

  int mtx_port = 0;
  const int probe = listen_loopback(mtx_port);
  ::close(probe);
  mediamtx mtx = start_mediamtx(dir, mtx_port);
  const bool up = wait_accepting(mtx.port, std::chrono::seconds {10});
  expect(up, "mediamtx listens on its RTMP port");
  std::printf("sink: mediamtx rtmp://127.0.0.1:%d\n", mtx.port);

  // A stall of 3 s of forwarding: long enough for the RTMP handshake and the
  // publish exchange, short enough that the run stays inside a test timeout.
  stall_proxy proxy {"127.0.0.1", mtx.port, std::chrono::seconds {3}};

  ts_ring ring {ts_ring::size_for_bandwidth(12000)};

  // Captured as well as printed: the assertions about *why* the output was
  // rebuilt are made on its own log, not on a guess from the counters.
  std::mutex log_mutex;
  std::vector<std::string> log_lines;
  auto log_line = [&log_mutex, &log_lines](const std::string& line) -> void
  {
    std::fputs(line.c_str(), stderr);
    std::lock_guard<std::mutex> guard(log_mutex);
    log_lines.push_back(line);
  };

  output_config stalled_cfg;
  stalled_cfg.id = "stalled";
  stalled_cfg.type = output_proto::rtmp;
  stalled_cfg.url = std::format("rtmp://127.0.0.1:{}/live/stall", proxy.port());
  stalled_cfg.host = "127.0.0.1";
  stalled_cfg.pinned_ip = "127.0.0.1";
  stalled_cfg.port = proxy.port();
  stalled_cfg.path = "live";
  stalled_cfg.key_or_streamid = "stall";

  output_config healthy_cfg = stalled_cfg;
  healthy_cfg.id = "healthy";
  healthy_cfg.url = std::format("rtmp://127.0.0.1:{}/live/healthy", mtx.port);
  healthy_cfg.port = mtx.port;
  healthy_cfg.key_or_streamid = "healthy";

  output stalled {stalled_cfg, ring, nullptr, log_line};
  output healthy {healthy_cfg, ring, nullptr, log_line};
  expect(stalled.start(), "stalled output starts");
  expect(healthy.start(), "healthy output starts");

  const auto started = std::chrono::steady_clock::now();
  const std::int64_t started_ms = now_ms();
  const auto run_until = started + std::chrono::seconds {20};

  std::uint64_t level_max = 0;
  guint64 cap = 0;
  // The pin is a *session*, not a moment: the queue also fills while the RTMP
  // handshake is in flight and drains as soon as the sink starts reading. The
  // clock the watchdog runs on starts at the pin that does not drain.
  bool pinned = false;
  std::int64_t pin_at = 0;
  std::int64_t rebuild_at = 0;
  bool stalled_rebuilt = false;
  std::uint64_t healthy_baseline = 0;
  bool healthy_baseline_taken = false;
  std::uint64_t healthy_published_at_stall = 0;

  std::thread sampler(
      [&]
      {
        while (std::chrono::steady_clock::now() < run_until) {
          const guint64 level = appsrc_level(stalled, cap);
          if (level > level_max) {
            level_max = level;
          }
          // "Pinned at the cap" is the moment the watchdog clock starts: the
          // gate only engages once the queue has stopped draining.
          if (cap != 0 && level >= cap && !pinned) {
            pinned = true;
            pin_at = now_ms();
          } else if (pinned && level < cap / 2) {
            pinned = false;
          }
          if (!healthy_baseline_taken && healthy.bytes_published_total() > 0) {
            healthy_baseline_taken = true;
            healthy_baseline = healthy.bytes_published_total();
          }
          if (pinned && healthy_published_at_stall == 0) {
            healthy_published_at_stall = healthy.bytes_published_total();
          }
          // A rebuild is either a new attempt (the reconnect counter) or the
          // output reaching its reconnecting state.
          if (stalled.reconnects() > 0
              || stalled.state() == output::run_state::reconnecting)
          {
            stalled_rebuilt = true;
            rebuild_at = now_ms();
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      });

  feed(ring, clip, run_until);
  sampler.join();
  const auto finished = std::chrono::steady_clock::now();

  const std::uint64_t stalled_published = stalled.bytes_published_total();
  const std::uint64_t healthy_published = healthy.bytes_published_total();
  stalled.stop();
  healthy.stop();

  std::printf("stalled: published=%llu bytes reconnects=%llu\n",
              static_cast<unsigned long long>(stalled_published),
              static_cast<unsigned long long>(stalled.reconnects()));
  std::printf("healthy: published=%llu bytes reconnects=%llu\n",
              static_cast<unsigned long long>(healthy_published),
              static_cast<unsigned long long>(healthy.reconnects()));
  std::printf("appsrc: cap=%llu max_level=%llu bytes (over cap by %lld)\n",
              static_cast<unsigned long long>(cap),
              static_cast<unsigned long long>(level_max),
              static_cast<long long>(level_max)
                  - static_cast<long long>(cap));

  bool ok = true;
  const auto fail = [&ok](const std::string& what)
  {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ok = false;
  };

  // (a) The stalled output was rebuilt inside the watchdog window.
  expect(pin_at != 0, "the stalled output pinned its appsrc at its cap");
  const std::int64_t held_ms = stalled_rebuilt ? rebuild_at - pin_at : -1;
  if (stalled_rebuilt) {
    std::printf("stalled: appsrc pinned at +%lld ms, rebuilt at +%lld ms "
                "(%lld ms held)\n",
                static_cast<long long>(pin_at - started_ms),
                static_cast<long long>(rebuild_at - started_ms),
                static_cast<long long>(held_ms));
  } else {
    std::printf("stalled: appsrc pinned at +%lld ms, never rebuilt\n",
                static_cast<long long>(pin_at - started_ms));
  }
  bool logged_stall = false;
  {
    std::lock_guard<std::mutex> guard(log_mutex);
    for (const std::string& line : log_lines) {
      if (line.find("sink stalled") != std::string::npos) {
        logged_stall = true;
        std::printf("stalled: %s", line.c_str());
      }
    }
  }
  if (!logged_stall) {
    fail("(a) the output never reported a stalled sink");
  }
  if (!stalled_rebuilt) {
    fail("(a) a stalled copy output was never rebuilt");
  } else if (held_ms > 8000) {
    fail(std::format("(a) rebuild took {} ms after the queue pinned (the stall "
                     "window is 5 s plus one rebuild)",
                     held_ms));
  }

  // (b) The queue stayed bounded: the cap plus at most one push (32 KiB chunk).
  if (cap == 0) {
    fail("(b) the appsrc cap was never read");
  } else if (level_max > cap + 65536) {
    fail(std::format("(b) appsrc level reached {} bytes against a {} byte cap: "
                     "the copy feed is unbounded",
                     level_max, cap));
  }

  // (c) The healthy output never noticed.
  expect(healthy_baseline != 0, "the healthy output published at all");
  if (!healthy_baseline_taken || healthy_published <= healthy_baseline) {
    fail("(c) the healthy output stopped publishing while the other stalled");
  }
  if (healthy.reconnects() != 0) {
    fail("(c) the healthy output reconnected: the stall cost another output");
  }
  if (healthy_published_at_stall != 0
      && healthy_published <= healthy_published_at_stall)
  {
    fail("(c) the healthy output published nothing after the stall began");
  }

  std::printf("elapsed: %lld ms\n",
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      finished - started)
                      .count()));

  stop_mediamtx(mtx);
  std::filesystem::remove_all(dir);
  if (!ok) {
    return 1;
  }
  std::printf("copy_stall_test: all assertions passed\n");
  return 0;
}
