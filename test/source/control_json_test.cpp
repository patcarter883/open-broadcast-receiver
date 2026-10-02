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
  stat.bytes_sent = 1234;
  stat.dropped_bytes = 5;
  stat.audio_dropped = false;
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
    expect(stats["bytes_sent"] == 1234 && stats["dropped_bytes"] == 5,
           "stats keeps byte counters");
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
  }

  // An encoder not yet chosen serialises as an empty string (never a crash).
  {
    output_stat stat = make_stat();
    stat.transcode = transcode_target::h265;
    const json status = json::parse(output_stat_json(stat, false));
    expect(status["transcode"]["encoder"] == "", "unbuilt encoder is empty");
    expect(status["transcode"]["frames_dropped"] == 0, "zero drops default");
  }

  std::puts("control_json_test: ALL OK");
  return 0;
}
