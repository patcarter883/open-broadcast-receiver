// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// abr_feed — the bonding rig's adaptive-bitrate sender.
//
// WHY THIS EXISTS: rist_feed reads stdin, so the encoder runs in ANOTHER
// process and nothing can retune it. Adaptation cannot be observed through a
// pipe, so the rig could not test ABR at all. abr_feed owns both halves in one
// process, exactly as the encoder does:
//
//   videotestsrc -> x264enc(name=enc) -> h264parse -> mpegtsmux -> appsink
//                      ^                                            |
//                      |            RISTNetSender.sendData <---------+
//                      |
//        g_object_set(enc,"bitrate") <- bitrate_scale::scale_locked()
//                                             ^
//                                   quality (source-dependent, below)
//
// The ABR step is the ENCODER'S OWN bitrate_scale.cpp, LINKED IN — not a copy.
// A copy would pass while production drifted. See
// open-broadcast-encoder/source/stats/bitrate_scale.h.
//
// ============================ SOURCE SELECTION ============================
// Mirrors encode_config.scaling_source, because the two are mutually exclusive
// in production as well (stats::got_rist_statistics implements ONLY the local
// branch; main.cpp's OOB handler is the only thing that scales in remote_oob).
//
//   ABR_SOURCE=oob   (DEFAULT) quality = the RECEIVER's link_quality, arriving
//                    as the 5-byte wan_telemetry over RIST OOB. This is the
//                    ONLY mode that works through a rist2rist bridge: the
//                    encoder's own RIST peer is then the bridge on the local
//                    LAN, so the sender-side figure measures a clean hop and
//                    reads 100 whatever the WAN is doing.
//   ABR_SOURCE=local quality = the sender's own sender_peer.quality. Correct
//                    only when the encoder's RIST peer IS the far end.
//
// The figure carried over OOB is receiver_flow.quality = received/(received +
// missing), and librist increments `missing` when a NACK is first queued
// (rist-common.c:1054) and never decrements it on recovery — so it DOES fall
// when packets need retransmitting, which is the wanted behaviour. It also
// counts reordering as missing, so on a bonded link it reads pessimistic.
//
// Usage: abr_feed <host> <port> [ceil_kbps [start_kbps [csv_out [secs]]]]
//   ABR_ALGO=off      -> fixed bitrate: the control run. Same pipeline, same
//                        link, no scaling, so a bitrate change is attributable
//                        to the ABR step and nothing else.
//   ABR_SOURCE=oob|local
//
// CSV, one row per ABR sample:
//   t_ms,src,quality,sent,retransmitted,bandwidth_kbps,bitrate_kbps,changed

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "RISTNet.h"
#include "lib/lib.h"
#include "stats/bitrate_scale.h"

namespace
{

constexpr std::size_t k_chunk = 7 * 188;  // 1316 B — the production datagram
constexpr std::size_t k_telemetry_len = 5;

enum class abr_source
{
  oob,
  local
};

struct abr_state
{
  RISTNetSender* sender = nullptr;
  GstElement* enc = nullptr;
  GstElement* sink = nullptr;
  cumulative_stats stats;
  encode_config cfg;
  abr_source source = abr_source::oob;
  std::atomic<bool> abr {true};
  std::atomic<bool> running {true};
  std::atomic<int> changes {0};
  std::atomic<int> oob_seen {0};
  std::FILE* csv = nullptr;
  int64_t t0_ms = 0;
};

auto now_ms() -> int64_t
{
  return g_get_monotonic_time() / 1000;
}

// The single ABR entry point, reached from whichever source is selected — just
// as in the encoder, where only one of the two paths runs.
void apply_quality(abr_state* f,
                   double quality,
                   int sent,
                   int retrans,
                   int bandwidth,
                   const char* tag)
{
  int new_bitrate = 0;
  {
    std::lock_guard<std::mutex> guard(f->stats.mutex);
    if (f->stats.current_bitrate == 0) {
      f->stats.current_bitrate =
          f->cfg.bitrate.load(std::memory_order_relaxed);
    }
    if (f->abr.load(std::memory_order_relaxed)) {
      bitrate_scale::scale_locked(quality, &f->stats, f->cfg, &new_bitrate);
    }
  }

  if (new_bitrate > 0 && f->enc != nullptr) {
    g_object_set(f->enc, "bitrate", new_bitrate, nullptr);
    f->changes.fetch_add(1, std::memory_order_relaxed);
  }

  if (f->csv != nullptr) {
    int shown = 0;
    {
      std::lock_guard<std::mutex> guard(f->stats.mutex);
      shown = f->stats.current_bitrate;
    }
    std::fprintf(f->csv, "%lld,%s,%.2f,%d,%d,%d,%d,%d\n",
                 static_cast<long long>(now_ms() - f->t0_ms), tag, quality, sent,
                 retrans, bandwidth, shown, new_bitrate > 0 ? 1 : 0);
    std::fflush(f->csv);
  }
}

// Sender-side path (ABR_SOURCE=local). On librist's RIST thread.
void on_stats(abr_state* f, const rist_stats& s)
{
  if (f->source != abr_source::local) {
    return;  // remote_oob: production scales from OOB only, not from here
  }
  apply_quality(f, static_cast<double>(s.stats.sender_peer.quality),
                static_cast<int>(s.stats.sender_peer.sent),
                static_cast<int>(s.stats.sender_peer.retransmitted),
                static_cast<int>(s.stats.sender_peer.bandwidth), "local");
}

// Receiver-side path (ABR_SOURCE=oob). The 5-byte wan_telemetry the receiver
// sends back — lib.h:57 {uint8 link_quality; uint32 worst_case_rtt;} packed.
void on_oob(abr_state* f, const uint8_t* buf, size_t size)
{
  // EXACT size, not >=. A rist2rist bridge carries its OWN OOB traffic (auth and
  // RTT/api messages — e.g. "auth,10.231.110.3:35329,0.0.0.0:6000"), and those
  // arrive in this same callback. Accepting anything >= 5 bytes let those be
  // scored as quality (buf[0] = 'a' = 97 -> 100), which is how a run through the
  // bridge produced a handful of bogus samples. The receiver always sends the
  // packed 5-byte wan_telemetry, so anything else is not telemetry.
  if (f->source != abr_source::oob || size != k_telemetry_len) {
    if (f->source == abr_source::oob && std::getenv("ABR_OOB_TRACE") != nullptr) {
      std::fprintf(stderr,
                   "abr_feed: oob NON-telemetry size=%zu first=%u bytes="
                   "%.*s\n",
                   size, size > 0 ? buf[0] : 0, static_cast<int>(size > 24 ? 24 : size),
                   reinterpret_cast<const char*>(buf));
    }
    return;
  }
  const double link_quality = static_cast<double>(buf[0]);
  uint32_t rtt_be = 0;
  std::memcpy(&rtt_be, buf + 1, sizeof(rtt_be));
  const uint32_t worst_rtt = ntohl(rtt_be);

  f->oob_seen.fetch_add(1, std::memory_order_relaxed);
  apply_quality(f, link_quality, 0, 0, static_cast<int>(worst_rtt), "oob");
}

}  // namespace

auto main(int argc, char** argv) -> int
{
  if (argc < 3) {
    std::fprintf(
        stderr,
        "usage: %s <host> <port> [ceil_kbps [start_kbps [csv_out [secs]]]]\n"
        "       ABR_SOURCE=oob|local   ABR_ALGO=off\n",
        argv[0]);
    return 2;
  }

  const std::string host = argv[1];
  const int port = std::atoi(argv[2]);
  const int ceil_kbps = argc > 3 ? std::atoi(argv[3]) : 40000;
  const int start_kbps = argc > 4 ? std::atoi(argv[4]) : ceil_kbps;
  const char* csv_path = argc > 5 ? argv[5] : nullptr;
  const int seconds = argc > 6 ? std::atoi(argv[6]) : 0;  // 0 = until stopped

  gst_init(&argc, &argv);

  abr_state f;
  f.cfg.bitrate.store(ceil_kbps, std::memory_order_relaxed);  // the CEILING
  f.stats.current_bitrate = start_kbps;
  f.stats.previous_quality = 0.0;
  if (const char* src = std::getenv("ABR_SOURCE");
      src != nullptr && std::strcmp(src, "local") == 0) {
    f.source = abr_source::local;
  }
  if (const char* algo = std::getenv("ABR_ALGO");
      algo != nullptr && std::strcmp(algo, "off") == 0) {
    f.abr.store(false, std::memory_order_relaxed);
  }
  if (csv_path != nullptr) {
    f.csv = std::fopen(csv_path, "w");
    if (f.csv == nullptr) {
      std::fprintf(stderr, "abr_feed: cannot open %s\n", csv_path);
      return 1;
    }
    std::fprintf(f.csv,
                 "t_ms,src,quality,sent,retransmitted,bandwidth_kbps,"
                 "bitrate_kbps,changed\n");
  }

  RISTNetSender sender;
  RISTNetSender::RISTNetSenderSettings settings;
  settings.mProfile = RIST_PROFILE_ADVANCED;  // must match the receiver
  settings.mLogLevel = RIST_LOG_INFO;
  const std::string url =
      "rist://" + host + ":" + std::to_string(port) + "?timing-mode=0";
  std::vector<std::tuple<std::string, int>> peers {{url, 5}};
  if (!sender.initSender(peers, settings)) {
    std::fprintf(stderr, "abr_feed: initSender failed\n");
    return 1;
  }
  f.sender = &sender;
  sender.statisticsCallback = [&f](const rist_stats& s) { on_stats(&f, s); };
  sender.networkOOBDataCallback =
      [&f](const uint8_t* buf,
           size_t size,
           std::shared_ptr<RISTNetSender::NetworkConnection>& /*conn*/,
           rist_peer* /*peer*/) { on_oob(&f, buf, size); };

  const std::string desc =
      "videotestsrc is-live=true pattern=snow ! videoconvert ! "
      "x264enc name=enc tune=zerolatency speed-preset=ultrafast "
      "key-int-max=50 bitrate=" + std::to_string(start_kbps) + " ! "
      "h264parse config-interval=1 ! mpegtsmux alignment=7 ! "
      "appsink name=sink max-buffers=120 drop=false sync=false";
  GError* gerr = nullptr;
  GstElement* pipeline = gst_parse_launch(desc.c_str(), &gerr);
  if (pipeline == nullptr) {
    std::fprintf(stderr,
                 "abr_feed: pipeline: %s\n",
                 gerr != nullptr ? gerr->message : "unknown");
    return 1;
  }
  f.enc = gst_bin_get_by_name(GST_BIN(pipeline), "enc");
  f.sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  if (f.enc == nullptr || f.sink == nullptr) {
    std::fprintf(stderr, "abr_feed: pipeline is missing enc/appsink\n");
    return 1;
  }
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE) {
    std::fprintf(stderr, "abr_feed: pipeline refused PLAYING\n");
    return 1;
  }

  std::fprintf(stderr,
               "abr_feed: -> %s:%d source=%s abr=%s ceil=%d start=%d\n",
               host.c_str(), port,
               f.source == abr_source::oob ? "oob" : "local",
               f.abr.load() ? "on" : "off", ceil_kbps, start_kbps);

  f.t0_ms = now_ms();
  std::vector<uint8_t> buf(k_chunk);
  std::size_t fill = 0;

  while (f.running.load(std::memory_order_relaxed)) {
    if (seconds > 0
        && now_ms() - f.t0_ms > static_cast<int64_t>(seconds) * 1000) {
      break;
    }
    GstSample* sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(f.sink), 100 * GST_MSECOND);
    if (sample == nullptr) {
      continue;  // timeout tick: re-check the clock
    }
    GstBuffer* gbuf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gbuf != nullptr && gst_buffer_map(gbuf, &map, GST_MAP_READ)) {
      for (std::size_t off = 0; off < map.size; off += k_chunk) {
        const std::size_t n = std::min(k_chunk, map.size - off);
        if (fill == 0 && n == k_chunk) {
          sender.sendData(map.data + off, n);  // aligned: send without a copy
          continue;
        }
        std::memcpy(buf.data() + fill, map.data + off, n);
        fill += n;
        if (fill == k_chunk) {
          sender.sendData(buf.data(), fill);
          fill = 0;
        }
      }
      gst_buffer_unmap(gbuf, &map);
    }
    gst_sample_unref(sample);
  }

  std::fprintf(stderr, "abr_feed: done, %d bitrate changes, %d OOB telemetry\n",
               f.changes.load(), f.oob_seen.load());
  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (f.csv != nullptr) {
    std::fclose(f.csv);
  }
  return 0;
}
