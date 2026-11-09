// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for the per-output /status and /stats serialisation (MT1.8): a
// transcode output reports a "transcode" object {codec, encoder,
// frames_dropped}; a copy-only output omits it entirely. Framework-free;
// exit 0 = pass. Uses nlohmann/json (no GStreamer).

#include <cstdio>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include "control/control.h"

using json = nlohmann::json;

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

auto make_stat() -> output_stat
{
  output_stat stat;
  stat.id = "yt";
  stat.type = "rtmp";
  stat.state = "running";
  stat.connected_s = 12;
  stat.reconnects = 2;
  stat.bytes_published_total = 1234;
  stat.frames_published_total = 89;
  stat.bytes_pushed = 98765;
  stat.dropped_bytes = 5;
  stat.audio_dropped = false;
  stat.audio_frames = 3392;
  return stat;
}
}  // namespace

auto main() -> int
{
  // Transcode output: the object carries codec/encoder/frames_dropped.
  {
    output_stat stat = make_stat();
    stat.transcode = transcode_target::h264;
    stat.transcode_encoder = "vah264enc";
    stat.frames_dropped = 7;

    const json status = json::parse(output_stat_json(stat, false));
    expect(status.contains("transcode"), "status has transcode object");
    expect(status["transcode"]["codec"] == "h264", "status codec");
    expect(status["transcode"]["encoder"] == "vah264enc", "status encoder");
    expect(status["transcode"]["frames_dropped"] == 7, "status frames_dropped");
    expect(status["type"] == "rtmp" && status["connected_s"] == 12,
           "status keeps copy fields");

    const json stats = json::parse(output_stat_json(stat, true));
    expect(stats["transcode"]["encoder"] == "vah264enc", "stats encoder");
    expect(stats["transcode"]["codec"] == "h264", "stats codec is h264");
    expect(stats["bytes_published_total"] == 1234
               && stats["frames_published_total"] == 89,
           "stats reports sink truth");
    expect(stats["bytes_sent"] == stats["bytes_published_total"],
           "bytes_sent carries the same sink-pad measure as bytes_published_total");
    expect(stats["dropped_bytes"] == 5, "stats keeps byte counters");
    expect(stats["bytes_pushed"] == 98765,
           "stats reports appsrc pushes separately from socket egress");
    expect(!stats.contains("type"), "stats omits type");
  }

  // Copy-only output: NO transcode object in either view.
  {
    output_stat stat = make_stat();
    const json status = json::parse(output_stat_json(stat, false));
    const json stats = json::parse(output_stat_json(stat, true));
    expect(!status.contains("transcode"), "copy-only status omits transcode");
    expect(!stats.contains("transcode"), "copy-only stats omits transcode");
    expect(status["audio_dropped"] == false, "status audio_dropped present");
    expect(status["audio_frames"] == 3392,
           "status audio_frames reports what the muxer's audio pad received");
  }

  // Every target names itself: an AV1 transcode must not surface as "none",
  // which reads as copy-only to every consumer of /stats and /status.
  {
    output_stat stat = make_stat();
    stat.transcode = transcode_target::av1;
    const json status = json::parse(output_stat_json(stat, false));
    const json stats = json::parse(output_stat_json(stat, true));
    expect(status["transcode"]["codec"] == "av1", "status codec is av1");
    expect(stats["transcode"]["codec"] == "av1", "stats codec is av1");
  }

  // An encoder not yet chosen serialises as an empty string (never a crash).
  {
    output_stat stat = make_stat();
    stat.transcode = transcode_target::h265;
    const json status = json::parse(output_stat_json(stat, false));
    expect(status["transcode"]["encoder"] == "", "unbuilt encoder is empty");
    expect(status["transcode"]["frames_dropped"] == 0, "zero drops default");
  }

  // ---- standby object (plan D3, CONTRACT §5) ------------------------------
  // An armed slate: every member the plan specifies, same in both views.
  {
    standby_stat stat;
    stat.enabled = true;
    stat.mode = "slate";
    stat.switches = 3;
    stat.last_live_ms_ago = 812;
    stat.caps_ok = true;
    stat.stage_rebuilds = 0;
    stat.degraded = "";

    const json status = json::parse(standby_stat_json(stat, false));
    const json stats = json::parse(standby_stat_json(stat, true));
    expect(status["enabled"] == true, "standby.enabled echoes the session");
    expect(status["mode"] == "slate", "standby.mode names the live source");
    expect(status["switches"] == 3, "standby.switches counts transitions");
    expect(status["last_live_ms_ago"] == 812, "standby age of last live");
    expect(status["caps_ok"] == true, "standby.caps_ok");
    expect(status["stage_rebuilds"] == 0, "standby.stage_rebuilds");
    expect(status["degraded"] == "", "healthy standby has an empty degraded");
    expect(status == stats, "/status and /stats carry the same standby object");
    expect(status.size() == 7,
           "the standby object has exactly the plan's members");
  }

  // A degraded slate carries the reason verbatim (render failure, or the
  // geometry fallback), and "switching" is expressible as a mode.
  {
    standby_stat stat;
    stat.enabled = true;
    stat.mode = "switching";
    stat.caps_ok = false;
    stat.degraded = "geometry defaulted";
    const json status = json::parse(standby_stat_json(stat, false));
    expect(status["mode"] == "switching", "mode can be switching");
    expect(status["degraded"] == "geometry defaulted",
           "degraded carries the reason, not a bool");
    expect(status["caps_ok"] == false,
           "caps_ok false when the branches differ");

    standby_stat failed;
    failed.enabled = true;
    failed.degraded = "no encoder for h264";
    const json s2 = json::parse(standby_stat_json(failed, true));
    expect(s2["degraded"] == "no encoder for h264",
           "a render failure reason survives serialisation");
    expect(s2["mode"] == "live", "default mode is live");
  }

  // Standby disabled (or stopped): present, and honest about being off.
  {
    const standby_stat stat;
    const json status = json::parse(standby_stat_json(stat, false));
    expect(status["enabled"] == false,
           "disabled standby reports enabled=false");
    expect(status["mode"] == "live", "disabled standby is live");
    expect(status["switches"] == 0 && status["stage_rebuilds"] == 0,
           "disabled standby has zeroed counters");
    expect(status["degraded"] == "", "disabled standby is not degraded");
  }

  std::puts("control_json_test: ALL OK");
  return 0;
}
