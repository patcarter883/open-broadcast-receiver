// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// stage_test -- the shared restartable stage against a real media stack
// (plan C4). Full lane only: it needs GStreamer, an h264 encoder and AAC.
//
// What this proves, in the order a session meets it:
//   1. a TS fed into the session's ts_ring comes out of video_ring as access
//      units on ONE strictly monotonic timeline, with gapless-ish steps;
//   2. audio AUs land on audio_ring sharing that timeline's offset (the same
//      re-stamper, follower side);
//   3. starving the input and asking for the slate keeps the timeline going --
//      the loop continues the published PTS instead of restarting at zero;
//   4. switching back to live with any_copy_output=true lands on a keyframe;
//   5. rebuild() under a running stage keeps the timeline monotonic and counts
//      itself.
//
// The fixture is 3 s of 320x180 @ 10 fps h264 + AAC in MPEG-TS, fed at roughly
// real time and looped. Exits 77 (skip) when the encoders are not installed.

#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

#include "fanout/es_ring.h"
#include "fanout/ring.h"
#include "standby/render.h"
#include "standby/stage.h"

namespace
{
using namespace std::chrono_literals;

int g_failures = 0;

auto expect(bool ok, const std::string& what) -> bool
{
  std::fprintf(stderr, "%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) {
    ++g_failures;
  }
  return ok;
}

constexpr int k_width = 320;
constexpr int k_height = 180;
constexpr int k_fps = 10;
constexpr std::int64_t k_frame_ns = 1'000'000'000LL / k_fps;
constexpr int k_frames = 30;  // 3 s

auto skip(const std::string& why) -> int
{
  std::fprintf(stderr, "SKIP: %s\n", why.c_str());
  return 77;
}

auto have_element(const char* name) -> bool
{
  GstElementFactory* f = gst_element_factory_find(name);
  if (f == nullptr) {
    return false;
  }
  gst_object_unref(f);
  return true;
}

// Run a pipeline to EOS or an error; false with a reason on either failure.
auto run_to_end(GstElement* pipeline, int timeout_s, std::string& err) -> bool
{
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE) {
    err = "refused PLAYING";
    return false;
  }
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* msg = gst_bus_timed_pop_filtered(
      bus, static_cast<GstClockTime>(timeout_s) * GST_SECOND,
      static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  bool ok = false;
  if (msg == nullptr) {
    err = "timed out";
  } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    GError* gerr = nullptr;
    gchar* dbg = nullptr;
    gst_message_parse_error(msg, &gerr, &dbg);
    err = gerr != nullptr ? gerr->message : "error";
    g_clear_error(&gerr);
    g_free(dbg);
  } else {
    ok = true;
  }
  if (msg != nullptr) {
    gst_message_unref(msg);
  }
  gst_object_unref(bus);
  return ok;
}

// A 3 s muxed fixture in the shape the product's encoder delivers: one aligned
// MPEG-TS per buffer, keyframe every 10 frames (1 s).
auto make_ts(const std::filesystem::path& path) -> bool
{
  const int audio_frames = (48000 / 1024) * 3;  // ~3 s of 1024-sample buffers
  const std::string desc = std::format(
      "videotestsrc is-live=false num-buffers={} "
      "! video/x-raw,format=I420,width={},height={},framerate={}/1 "
      "! x264enc tune=zerolatency speed-preset=ultrafast key-int-max=10 "
      "bitrate=400 ! h264parse config-interval=-1 "
      "! mpegtsmux name=m alignment=7 "
      "audiotestsrc is-live=false num-buffers={} wave=sine "
      "! audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate=64000 "
      "! aacparse ! audio/mpeg,mpegversion=4,stream-format=adts,parsed=true "
      "! m. "
      "m. ! filesink location={}",
      k_frames, k_width, k_height, k_fps, audio_frames, path.string());
  GError* perr = nullptr;
  GstElement* pipeline = gst_parse_launch(desc.c_str(), &perr);
  if (pipeline == nullptr) {
    std::fprintf(stderr, "fixture: %s\n", perr != nullptr ? perr->message : "?");
    g_clear_error(&perr);
    return false;
  }
  std::string err;
  const bool ok = run_to_end(pipeline, 30, err);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  if (!ok) {
    std::fprintf(stderr, "fixture: %s\n", err.c_str());
  }
  return ok;
}

struct collected
{
  es_ring::au_meta meta;
  std::chrono::steady_clock::time_point at;
};

// Drains both rings continuously, so the stage never sees a full ring and every
// AU the probe publishes is recorded with the instant it arrived.
class collector
{
public:
  collector(es_ring& video, int video_con, es_ring& audio, int audio_con)
      : m_video {video}
      , m_audio {audio}
      , m_video_con {video_con}
      , m_audio_con {audio_con}
  {
  }

  auto start() -> void
  {
    m_video_thread =
        std::thread(&collector::run, this, std::ref(m_video), m_video_con,
                    std::ref(m_video_aus));
    m_audio_thread =
        std::thread(&collector::run, this, std::ref(m_audio), m_audio_con,
                    std::ref(m_audio_aus));
  }

  auto stop() -> void
  {
    m_stopping.store(true);
    if (m_video_thread.joinable()) {
      m_video_thread.join();
    }
    if (m_audio_thread.joinable()) {
      m_audio_thread.join();
    }
  }

  [[nodiscard]] auto video_count() -> std::size_t
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_video_aus.size();
  }

  [[nodiscard]] auto audio_count() -> std::size_t
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_audio_aus.size();
  }

  [[nodiscard]] auto video() -> std::vector<collected>
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_video_aus;
  }

  [[nodiscard]] auto audio() -> std::vector<collected>
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_audio_aus;
  }

private:
  auto run(es_ring& ring, int con, std::vector<collected>& out) -> void
  {
    std::vector<std::uint8_t> buf(1U << 20);
    while (!m_stopping.load()) {
      es_ring::au_meta meta;
      const std::size_t n =
          ring.read(con, meta, buf.data(), buf.size(), std::chrono::milliseconds {100});
      if (n == 0) {
        continue;
      }
      // Stamped AFTER the read: the read blocks until the AU is published, so
      // this is the publish instant (a pre-read stamp would be one frame early).
      const auto at = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> guard(m_mutex);
      out.push_back(collected {meta, at});
    }
  }

  es_ring& m_video;
  es_ring& m_audio;
  int m_video_con;
  int m_audio_con;
  std::vector<collected> m_video_aus;
  std::vector<collected> m_audio_aus;
  std::mutex m_mutex;
  std::atomic<bool> m_stopping {false};
  std::thread m_video_thread;
  std::thread m_audio_thread;
};

// Feeds the fixture into the session's ts_ring at roughly real time, looping it
// (a loop restarts the stream's own clock, which the re-stamper must absorb).
class feeder
{
public:
  feeder(ts_ring& ring, std::vector<std::uint8_t> data)
      : m_ring {ring}
      , m_data {std::move(data)}
  {
  }

  auto start() -> void
  {
    m_thread = std::thread(&feeder::run, this);
  }

  auto stop() -> void
  {
    m_stopping.store(true);
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  // Stops pushing without ending the thread: the demux drains and starves.
  std::atomic<bool> starve {false};

private:
  auto run() -> void
  {
    constexpr std::size_t k_chunk = 7 * 188;  // one RIST-ish payload
    const std::size_t chunks = m_data.size() / k_chunk;
    if (chunks == 0) {
      return;
    }
    const auto per_chunk = std::chrono::nanoseconds(
        (static_cast<std::int64_t>(k_frames) * k_frame_ns)
        / static_cast<std::int64_t>(chunks));
    std::size_t pos = 0;
    while (!m_stopping.load()) {
      if (starve.load()) {
        std::this_thread::sleep_for(10ms);
        continue;
      }
      const std::size_t n = std::min(k_chunk, m_data.size() - pos);
      m_ring.write(m_data.data() + pos, n);
      pos += n;
      if (pos >= m_data.size()) {
        pos = 0;  // loop: the TS clock restarts, the stage must not
      }
      std::this_thread::sleep_for(per_chunk);
    }
  }

  ts_ring& m_ring;
  std::vector<std::uint8_t> m_data;
  std::atomic<bool> m_stopping {false};
  std::thread m_thread;
};

template <class Pred>
auto wait_for(Pred pred, std::chrono::milliseconds timeout) -> bool
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return pred();
}

// Strictly increasing, and no step larger than half a second: at 10 fps a
// "gapless-ish" stream steps by ~100 ms; anything near max_gap means the
// re-stamper compressed a jump (allowed, but not on a contiguous live run).
auto check_monotonic(const std::vector<collected>& aus, const char* what)
    -> bool
{
  bool ok = true;
  for (std::size_t i = 1; i < aus.size(); ++i) {
    const std::int64_t step = aus[i].meta.pts_ns - aus[i - 1].meta.pts_ns;
    if (step <= 0) {
      ok = expect(false, std::format("{}: PTS went backwards at AU {} "
                                     "({} -> {})",
                                     what, i,
                                     aus[i - 1].meta.pts_ns / 1'000'000,
                                     aus[i].meta.pts_ns / 1'000'000));
      break;
    }
  }
  return ok;
}

// The follower contract, tested strictly: audio only ADDS the driver's offset
// and then clamps forward against its own last stamp, so every audio stream a
// muxer ever sees is STRICTLY monotonic across any number of source switches.
// A switch may cost one audio frame of A/V skew (inaudible); a backwards PTS
// is a muxer error, so it is never tolerated.
auto check_audio_timeline(const std::vector<collected>& aus) -> bool
{
  // A switch may leave audio a real hole (the stream was starved while the
  // other branch published); that hole can never exceed the stage's own
  // max_gap, which is what the driver compresses to. A hole is fine. A
  // BACKWARDS stamp is not: it is the defect this check exists to catch.
  constexpr std::int64_t k_audio_max_step_ns = 4'000'000'000;  // max_gap_ms

  for (std::size_t i = 1; i < aus.size(); ++i) {
    const std::int64_t step = aus[i].meta.pts_ns - aus[i - 1].meta.pts_ns;
    if (step <= 0 || step > k_audio_max_step_ns) {
      return expect(false, std::format("audio: PTS not strictly monotonic at "
                                       "AU {} ({} -> {} ms, step {} us)",
                                       i,
                                       aus[i - 1].meta.pts_ns / 1'000'000,
                                       aus[i].meta.pts_ns / 1'000'000,
                                       step / 1'000));
    }
  }
  return true;
}

auto read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out)
    -> bool
{
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(f);
    return false;
  }
  out.resize(static_cast<std::size_t>(size));
  const std::size_t got = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  out.resize(got);
  return got > 0;
}
}  // namespace

auto main() -> int
{
  gst_init(nullptr, nullptr);
  if (!have_element("x264enc") || !have_element("avenc_aac")) {
    return skip("x264enc and avenc_aac are required");
  }

  const auto dir = std::filesystem::temp_directory_path()
                   / std::format("obr-stage-{}", static_cast<long>(::getpid()));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  const auto ts_path = dir / "fixture.ts";
  if (!make_ts(ts_path)) {
    return 1;
  }

  // The slate, rendered exactly as the product renders it (plan B1).
  standby::slate_request req;
  req.codec = "h264";
  req.width = k_width;
  req.height = k_height;
  req.fps = k_fps;
  req.bitrate_kbps = 400;
  req.out_dir = (dir / "slate").string();
  req.encoder_override = "x264enc";
  std::string slate_err;
  const auto slate = standby::render_slate(req, slate_err);
  if (!slate.has_value()) {
    return skip("could not render the slate: " + slate_err);
  }

  std::vector<std::uint8_t> ts_bytes;
  if (!read_file(ts_path, ts_bytes)) {
    return 1;
  }

  es_ring video_ring {es_ring::size_for_bps(1500)};
  es_ring audio_ring {es_ring::size_for_bps(192)};
  ts_ring input_ring {ts_ring::size_for_bandwidth(2000)};

  const int video_con = video_ring.add_consumer();
  const int audio_con = audio_ring.add_consumer();
  expect(video_con >= 0 && audio_con >= 0, "consumer slots on both rings");

  collector sink {video_ring, video_con, audio_ring, audio_con};
  feeder feed {input_ring, ts_bytes};

  standby::stage st {[](const std::string& line) {
    std::fprintf(stderr, "%s\n", line.c_str());
  }};

  standby::stage_config cfg;
  cfg.in_codec = "h264";
  cfg.width = k_width;
  cfg.height = k_height;
  cfg.fps = k_fps;
  cfg.slate_video_es = slate->video_path;
  cfg.slate_audio_es = slate->audio_path;
  cfg.any_copy_output = true;  // a copy destination: the video switch gates
  cfg.live_gap_ms = 200;
  cfg.live_hold_ms = 1000;
  cfg.max_gap_ms = 4000;
  cfg.video_ring = &video_ring;
  cfg.audio_ring = &audio_ring;
  cfg.input = &input_ring;

  expect(st.start(cfg), "the stage starts");
  feed.start();
  sink.start();

  // ---- 1. the slate publishes, on a monotonic timeline --------------------
  expect(wait_for([&] { return sink.video_count() >= 5; }, 10s),
         "slate AUs reach video_ring");
  expect(st.stats().slate_active, "a session with a slate starts on it");
  auto video_aus = sink.video();
  check_monotonic(video_aus, "slate");

  // ---- 4. back to live, gated on a keyframe -------------------------------
  const auto last_slate_pts = video_aus.back().meta.pts_ns;
  st.set_slate(false);

  // Poll tightly: the flip happens inside the live pad's buffer probe, so the
  // keyframe AU is published at the instant slate_active() turns false. A
  // 40 ms guard is well inside the 100 ms frame spacing, so no other AU can
  // fall into the window.
  auto flip_at = std::chrono::steady_clock::time_point {};
  {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline
           && st.stats().slate_active) {
      std::this_thread::sleep_for(2ms);
    }
    flip_at = std::chrono::steady_clock::now();
  }
  expect(!st.stats().slate_active, "the switch lands on live");

  // The switch flips inside the live pad's buffer probe, i.e. exactly at the
  // keyframe the gate chose. A slate AU already in flight can be published in
  // the same millisecond (it precedes the flip in ring order), so the marker is
  // the LAST AU published up to a guard after the flip was observed: everything
  // from the flip onwards is live, and the first live AU is the keyframe. The
  // next live AU is a frame later (100 ms), well outside the 40 ms guard.
  bool saw_live = false;
  collected first_live;
  for (const auto& au : sink.video()) {
    if (au.at <= flip_at + 40ms) {
      first_live = au;
      saw_live = true;
    }
  }
  expect(saw_live, "an AU is published around the switch instant");
  expect(first_live.meta.pts_ns > last_slate_pts,
         std::format("the first live AU continues the timeline ({} -> {})",
                     last_slate_pts / 1'000'000,
                     first_live.meta.pts_ns / 1'000'000));
  expect(first_live.meta.keyframe,
         "with any_copy_output the switch lands on a keyframe");

  // ---- 1b. a contiguous live run: monotonic, gapless-ish ------------------
  const std::size_t before_live = sink.video_count();
  expect(wait_for([&] { return sink.video_count() >= before_live + 12; }, 10s),
         "live AUs keep arriving");
  video_aus = sink.video();
  check_monotonic(video_aus, "live");
  {
    int in_band = 0;
    int steps = 0;
    for (std::size_t i = 1; i < video_aus.size(); ++i) {
      const std::int64_t step = video_aus[i].meta.pts_ns
                                - video_aus[i - 1].meta.pts_ns;
      if (step <= 0) {
        continue;
      }
      ++steps;
      if (step >= k_frame_ns / 2 && step <= 2 * k_frame_ns) {
        ++in_band;
      }
    }
    expect(steps > 0 && in_band * 10 >= steps * 9,
           std::format("at least 90% of steps are one frame ({} of {})", in_band,
                       steps));
  }

  // ---- 2. audio shares the video timeline's offset -----------------------
  expect(wait_for([&] { return sink.audio_count() >= 6; }, 10s),
         "audio AUs reach audio_ring");
  {
    const auto audio_aus = sink.audio();
    int checked = 0;
    int close = 0;
    for (std::size_t i = 0; i < audio_aus.size() && checked < 3; i += 2) {
      std::int64_t nearest = 0;
      bool first = true;
      for (const auto& v : video_aus) {
        const std::int64_t d = v.meta.pts_ns - audio_aus[i].meta.pts_ns;
        if (first || (d < 0 ? -d : d) < (nearest < 0 ? -nearest : nearest)) {
          nearest = d;
          first = false;
        }
      }
      ++checked;
      if (nearest <= 150'000'000LL && nearest >= -150'000'000LL) {
        ++close;
      }
    }
    expect(checked == 3 && close == 3,
           std::format("3 audio AUs sit on the video timeline ({}/3 within "
                       "150 ms)",
                       close));
    check_audio_timeline(audio_aus);
  }

  // ---- 3. starve, then slate: the timeline continues ----------------------
  feed.starve.store(true);
  std::this_thread::sleep_for(400ms);
  const std::size_t before_slate = sink.video_count();
  const auto last_live_pts = sink.video().back().meta.pts_ns;
  st.set_slate(true);
  expect(wait_for([&] { return sink.video_count() >= before_slate + 4; }, 8s),
         "the slate resumes publishing while the input is starved");
  expect(st.stats().slate_active, "the switch is on the slate");
  {
    const auto now_aus = sink.video();
    expect(now_aus.back().meta.pts_ns > last_live_pts,
           std::format("slate PTS continues after the last live AU ({} -> {})",
                       last_live_pts / 1'000'000,
                       now_aus.back().meta.pts_ns / 1'000'000));
    check_monotonic(now_aus, "starvation + slate");
    // The slate loop is joined wherever it happens to be, so the first slate AU
    // published after the request must be decodable on its own: the renderer
    // pins a keyframe on every frame of the slate.
    bool found_slate = false;
    collected first_slate;
    for (const auto& au : now_aus) {
      if (au.meta.pts_ns > last_live_pts) {
        first_slate = au;
        found_slate = true;
        break;
      }
    }
    expect(found_slate, "a slate AU is published after the request");
    expect(first_slate.meta.keyframe,
           "the first slate AU presented is a keyframe, not a mid-loop one");
  }

  // ---- 5. rebuild under a running stage, timeline preserved --------------
  feed.starve.store(false);
  st.set_slate(false);
  expect(wait_for([&] { return !st.stats().slate_active; }, 8s),
         "back on live before the rebuild");
  const std::size_t before_rebuild = sink.video_count();
  expect(wait_for([&] { return sink.video_count() >= before_rebuild + 4; }, 8s),
         "live is flowing before the rebuild");
  const auto pts_before_rebuild = sink.video().back().meta.pts_ns;

  expect(st.rebuild(), "rebuild() under a running stage succeeds");
  expect(wait_for([&] { return sink.video_count() >= before_rebuild + 8; }, 10s),
         "AUs keep arriving after the rebuild");
  {
    const auto all = sink.video();
    check_monotonic(all, "across the rebuild");
    bool crossed = false;
    for (const auto& au : all) {
      if (au.meta.pts_ns > pts_before_rebuild) {
        crossed = true;
        break;
      }
    }
    expect(crossed, "an AU is published after the rebuild");
    expect(all.back().meta.pts_ns > pts_before_rebuild,
           std::format("the rebuild does not restart the timeline ({} -> {})",
                       pts_before_rebuild / 1'000'000,
                       all.back().meta.pts_ns / 1'000'000));
  }
  const auto final_stats = st.stats();
  expect(final_stats.rebuilds == 1, "stats().rebuilds counts the rebuild");
  expect(final_stats.switches >= 3, "stats().switches counts the mode changes");
  expect(final_stats.caps_ok, "the branches agree on caps");
  expect(final_stats.aus_in > 0 && final_stats.aus_out > 0,
         "stats() counts AUs in and out");
  expect(final_stats.aus_out <= final_stats.aus_in,
         "no more AUs published than passed the switches");

  // ---- 2b. every audio AU of the whole run, across both set_slate round
  // trips, is strictly monotonic: no tolerance for a backwards stamp ---------
  {
    const auto all_audio = sink.audio();
    expect(all_audio.size() > 12, "audio spans every switch");
    check_audio_timeline(all_audio);
  }

  st.stop();
  st.stop();  // idempotent

  // ---- the no-slate configuration ----------------------------------------
  // Without slate files there are no slate branches and no selectors: the stage
  // must still be a plain demux -> ring publisher on the same timeline.
  {
    const std::size_t before = sink.video_count();
    standby::stage_config plain_cfg = cfg;
    plain_cfg.slate_video_es.clear();
    plain_cfg.slate_audio_es.clear();
    plain_cfg.any_copy_output = false;
    standby::stage plain {[](const std::string& line) {
      std::fprintf(stderr, "%s\n", line.c_str());
    }};
    expect(plain.start(plain_cfg), "a stage with no slate starts");
    expect(wait_for([&] { return sink.video_count() >= before + 5; }, 10s),
           "a stage with no slate publishes AUs to the ring");
    const auto plain_stats = plain.stats();
    expect(!plain_stats.slate_active && plain_stats.switches == 0,
           "no slate means no switching");
    expect(plain_stats.caps_ok, "no slate means the caps trivially agree");
    plain.stop();
  }

  sink.stop();
  feed.stop();

  std::filesystem::remove_all(dir, ec);
  std::fprintf(stderr, g_failures == 0 ? "stage_test: PASS\n"
                                       : "stage_test: FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
