// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_RELAY_RELAY_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_RELAY_RELAY_H

#include <atomic>
#include <string>

// Relay mode: receive on one `@`-bound RIST leg, send on another. This is the
// trunk bridge a BYON venue sender crosses when the node's receiver cannot be
// dialled directly.
//
// It is a PACKET PUMP. The payload is handed to the sender leg exactly as it
// arrived — no parse, no remux, no restamp, no dedupe — and the incoming
// sequence numbers are preserved (RIST_DATA_FLAGS_USE_SEQ, the same thing
// rist2rist does), so the far receiver sees the venue sender's sequence
// continuity rather than a fresh one. Anything that touches timestamps here
// re-opens the `timing-mode` class of bug.
//
// The relay runs inside the receiver image (one image to pin, one build to
// sign) but takes no control port and no GStreamer pipeline: `main` branches
// here before gst_init.
namespace relay
{

struct relay_config
{
  // `@`-bound receiver URL: this end binds and waits for the venue sender.
  std::string in_url;
  // `@`-bound sender URL: this end binds and waits for the node's receiver,
  // which then dials in. Bound on BOTH legs is what removes any need for
  // signalling or endpoint exchange — the listening sender learns its peer
  // from the first inbound packet, so the relay never needs the node's
  // address.
  std::string out_url;
  // SRP file for the sender leg (`ristsrppasswd` format). Empty = no SRP.
  std::string srp_path;
  // AES secret for the sender leg. Empty = no encryption.
  std::string secret;
  int aes = 256;  // 128 | 256
};

using log_fn = void (*)(const std::string&);

// Validate a relay config without touching the network. Fills `err` and
// returns false for: a missing leg, a leg without '@' (a relay must bind both,
// or it is not a relay), a missing SRP file, a bad AES size, or a secret that
// cannot fit librist's field.
auto validate_relay_config(const relay_config& cfg, std::string& err) -> bool;

// Run until SIGINT/SIGTERM. Returns 0 on a clean stop, non-zero when either leg
// fails to bind — a relay that half-bound is the worst outcome, because the
// venue's sender looks healthy while nothing is forwarded.
auto run_relay(const relay_config& cfg, log_fn log) -> int;

}  // namespace relay

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_RELAY_RELAY_H
