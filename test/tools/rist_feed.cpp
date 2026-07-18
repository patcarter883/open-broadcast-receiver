// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// rist_feed — test-rig traffic generator (FIXPLAN M1.6): reads an MPEG-TS
// byte stream on stdin and sends it over RIST (ADVANCED profile, SOURCE
// timing — matching the encoder and the receiver's listener) in 7×188-byte
// chunks. Pair with a live gst-launch mux writing to a pipe:
//
//   gst-launch-1.0 -q videotestsrc is-live=true ! x264enc tune=zerolatency \
//     ! h264parse config-interval=1 ! mpegtsmux alignment=7 ! fdsink fd=1 \
//     | rist_feed <host> <port>
//
// Usage: rist_feed <host> <port> [psk-hex [aes-bits]]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include <unistd.h>

#include "RISTNet.h"

auto main(int argc, char** argv) -> int
{
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <host> <port> [psk-hex [aes-bits]]\n",
                 argv[0]);
    return 2;
  }
  const std::string host = argv[1];
  const int port = std::atoi(argv[2]);

  RISTNetSender sender;
  RISTNetSender::RISTNetSenderSettings settings;
  settings.mProfile = RIST_PROFILE_ADVANCED;  // must match the receiver
  settings.mLogLevel = RIST_LOG_INFO;
  if (argc > 3) {
    settings.mPSK = argv[3];
  }

  // Peer list entries are (URL, weight). timing-mode=0 (SOURCE) end-to-end,
  // like the real encoder (CONTRACT §4).
  const std::string url =
      "rist://" + host + ":" + std::to_string(port) + "?timing-mode=0";
  std::vector<std::tuple<std::string, int>> peers {{url, 5}};
  if (!sender.initSender(peers, settings)) {
    std::fprintf(stderr, "rist_feed: initSender failed\n");
    return 1;
  }

  constexpr std::size_t k_chunk = 7 * 188;
  std::vector<uint8_t> buf(k_chunk);
  std::size_t fill = 0;
  for (;;) {
    const ssize_t got = ::read(0, buf.data() + fill, k_chunk - fill);
    if (got <= 0) {
      break;  // EOF or error: done
    }
    fill += static_cast<std::size_t>(got);
    if (fill == k_chunk) {
      sender.sendData(buf.data(), fill);
      fill = 0;
    }
  }
  if (fill > 0) {
    sender.sendData(buf.data(), fill);
  }
  return 0;
}
