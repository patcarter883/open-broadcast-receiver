// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_RELAY_RELAY_SRP_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_RELAY_RELAY_SRP_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// SRP credential file handling for the relay's authenticated sender leg.
//
// The file is the one `ristsrppasswd <user> <pass>` writes and every librist
// tool reads (`-F <file>`):
//
//     username:base64(verifier):base64(salt):3:0
//
// Field 4 is the line FORMAT version — the constant 3 in every file
// ristsrppasswd writes, carrying no information about the credential.
// Field 5 is the credential's hash version: `ristsrppasswd` calls
// librist_crypto_srp_create_verifier(..., correct) twice and prints
// `correct ? 1 : 0`, i.e. 0 = librist's pre-0.2.16 combined hash, 1 = the
// RFC 5054-correct hash every current peer computes. Both lines are written so
// peers on either build can authenticate, with DIFFERENT verifiers for the same
// password — so serving the wrong one is a failed handshake, not a slow one.
// (Measured on librist 0.2.20: the legacy line fails with
// `[EAP-SRP] Authentication failed`; the correct line authenticates.)
//
// librist's own tools carry a 200-line state machine for this (srp_shared.c);
// the relay needs "which verifier+salt do I hand librist for this user", which
// is a split on ':' plus a base64 decode.
//
// Everything here is pure: no librist, no sockets, no filesystem side effects
// beyond reading the file it is asked to read, so it unit-tests in the
// tests-only lane.
namespace relay
{

struct srp_credential
{
  std::string username;
  std::vector<std::uint8_t> verifier;
  std::vector<std::uint8_t> salt;
  // Credential hash version (field 5): 0 = legacy combined hash, 1 = the
  // RFC 5054-correct one. Files without field 5 read as 0.
  int hashversion = 0;
  auto operator==(const srp_credential&) const -> bool = default;
};

// Standard base64 (padded or unpadded) into `out`. False on any invalid byte.
auto base64_decode(std::string_view in, std::vector<std::uint8_t>& out) -> bool;

// Parse one line of an SRP file. Trailing fields beyond the hash version are
// ignored (ristsrppasswd appends a legacy flag). False when the line is blank,
// a comment, or missing a decodable verifier/salt.
auto parse_srp_line(std::string_view line, srp_credential& out) -> bool;

// Read and parse every usable line of `path`. Missing file / unreadable file
// yields an empty vector — the caller fails the bind rather than starting a
// sender leg that would authenticate nobody.
auto load_srp_file(const std::string& path) -> std::vector<srp_credential>;

// The entry to serve for `username`. `wanted_hashversion` is librist's IN/OUT
// value: the version the peer's handshake uses (1 for EAP v3 on mbedtls
// builds, 0 for the legacy path), answered with the version actually served —
// librist sets eapversion3 from it. Exact match first, else the newest entry
// below the request, else the oldest above it. nullptr when the user is absent.
auto find_srp_credential(const std::vector<srp_credential>& creds,
                         std::string_view username,
                         int wanted_hashversion) -> const srp_credential*;

}  // namespace relay

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_RELAY_RELAY_SRP_H
