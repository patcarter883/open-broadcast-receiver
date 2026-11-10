// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for the relay's SRP credential file parser.
//
// The lines used below are trimmed copies of real `ristsrppasswd` output: the
// tool emits one entry per hash version (legacy 0 and current 1) so peers on
// either librist build can authenticate. Getting the selection wrong refuses a
// real customer's stream at the sender leg, so the fallback is pinned here.
// Framework-free; exit 0 = pass.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "relay/relay_srp.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

// "alice:" + base64 verifier + ":" + base64 salt + ":3:0"
constexpr const char* k_line_v0 =
    "alice:WfVf2D9+XEaC5hxFpqWYSTDKN9wVrB4Wfb74jUivlc/cUERH2CSKRhMutsLCUI7iQz"
    "x+XJm59m6zNMDbGTOxVBgcWaM/C2JKd4NKf7mgfku1jRFqUWEMpjV6tGvDDjJc5JzHkwRcN"
    "cIh9nS15dV9hfX3npgiYaqU0FiN01fmScfaK9VNr3FROTR+E0PzJrKmCSDP/WlKPM+ohPhy"
    "4bADQrlCKfa5De07keGSfTKav8IEjfa085qFe5w7FMr6gz/iNFzkNAllns/Dq9NiEoZAXnW"
    "XeEtT3q19RZbkEWSQDj+oC9inKaUJU9uhl1OdAe87blSgXhRrYMyJT3XQoaevHw==:Uy8Uen"
    "YALxvWfMFLiiQhWhbCDFW8KGk0r/z2LBq+vv0=:3:0";
constexpr const char* k_line_v1 =
    "alice:J+zWil4t0OGYv4a0i/GsBUM+nEcfTFtFLp5wZui3VOnvz98sS0TvFFcpRwv/7UosPTj"
    "5hD4VaPWnVN8Ps/9HwAr7AGvUbZUWksL+6YEwjOTGB6O3dmAka7GeqLr8FFw++xqbjYtV/ay"
    "gX/uWb7wRhzIlWTOUODl8RcZfvN0Qpa6jwu+LQRu8MyiMg9Oycv3jmMfahLP8YXHO5/9h3dg"
    "vCZFbjPfakwDt3HKlJ6fq2UNhXbKRaCvGjYAIJaU7tzNCX/6SRTG8v9hSe+rQt216zJNtmgE"
    "QS5Sw79ggGMkGp3zPQxyyQBP0DWJj3St2UPKZt6HeIcKTINzyyVTRs6o91Q==:z1YHKHKdTUOu"
    "YZvosaZLEpoXwDjtjnjyrY4t9ZGJEK0=:3:1";

auto lines() -> std::vector<std::string>
{
  return {k_line_v0, k_line_v1};
}
}  // namespace

auto main() -> int
{
  using relay::parse_srp_line;
  using relay::srp_credential;

  // ---- base64 -------------------------------------------------------------
  {
    std::vector<std::uint8_t> out;
    expect(relay::base64_decode("aGVsbG8=", out), "base64 decodes padded");
    expect(std::string(out.begin(), out.end()) == "hello", "and yields hello");
    expect(relay::base64_decode("aGVsbG8", out), "unpadded decodes too");
    expect(std::string(out.begin(), out.end()) == "hello", "same bytes");
    expect(relay::base64_decode("", out) && out.empty(), "empty input is empty");
    expect(!relay::base64_decode("a$b=", out), "invalid byte is refused");
    expect(!relay::base64_decode("ab=c", out),
           "data after padding is refused");
    expect(!relay::base64_decode("a b=", out), "embedded space is refused");
  }

  // ---- line parsing -------------------------------------------------------
  {
    srp_credential cred;
    expect(parse_srp_line(k_line_v0, cred), "ristsrppasswd line parses");
    expect(cred.username == "alice", "username is field 1");
    expect(!cred.verifier.empty(), "verifier decoded");
    expect(cred.salt.size() == 32, "salt decoded (32 bytes)");
    // Field 4 is the constant line-format marker (3); field 5 is the credential
    // hash version — 0 here, librist's legacy combined hash.
    expect(cred.hashversion == 0, "legacy line reads as hash version 0");
    // sha384 verifier for a 2048-bit modulus is 384 bytes; the exact length is
    // the tool's output, not ours to invent — just prove it decoded fully.
    expect(cred.verifier.size() > 200, "verifier is a full-length value");
  }
  {
    // The second line is the same user and password computed with the
    // RFC 5054-correct hash — a DIFFERENT verifier for the same salt length.
    srp_credential cred;
    expect(parse_srp_line(k_line_v1, cred), "second line parses");
    expect(cred.hashversion == 1, "correct-hash line reads as version 1");
    expect(cred.username == "alice", "same username");
    expect(cred.salt.size() == 32, "same salt length");
  }
  {
    // CRLF and the legacy 3-field layout (no hash version).
    srp_credential cred;
    expect(parse_srp_line("bob:YWJj:ZGVm\r\n", cred), "CRLF 3-field line parses");
    expect(cred.username == "bob", "username bob");
    expect(cred.hashversion == 0, "missing hash version reads as 0");
    expect(std::string(cred.verifier.begin(), cred.verifier.end()) == "abc",
           "verifier bytes");
    expect(std::string(cred.salt.begin(), cred.salt.end()) == "def", "salt bytes");
  }
  {
    srp_credential cred;
    expect(!parse_srp_line("", cred), "blank line is skipped");
    expect(!parse_srp_line("# comment", cred), "comment line is skipped");
    expect(!parse_srp_line("alice:only-username", cred), "short line is skipped");
    expect(!parse_srp_line("alice:!!!:ZGVm:3", cred),
           "undecodable verifier is skipped");
    expect(!parse_srp_line("alice::ZGVm:3", cred), "empty verifier is skipped");
  }

  // ---- selection ----------------------------------------------------------
  {
    std::vector<srp_credential> creds;
    for (const std::string& line : lines()) {
      srp_credential cred;
      expect(parse_srp_line(line, cred), "seed line parses");
      creds.push_back(cred);
    }
    // librist asks for the version its handshake uses (1 on EAP v3). It must get
    // the CORRECT-hash entry: the legacy one is computed differently from the
    // same password and fails the handshake outright.
    const srp_credential* modern = relay::find_srp_credential(creds, "alice", 1);
    expect(modern != nullptr, "alice is found for a version-1 request");
    expect(modern->hashversion == 1, "the correct-hash entry is served");
    expect(modern == &creds.back(), "which is the second line of the pair");
    const srp_credential* legacy = relay::find_srp_credential(creds, "alice", 0);
    expect(legacy != nullptr && legacy->hashversion == 0,
           "a legacy peer gets the legacy entry");
    expect(legacy == &creds.front(), "which is the first line of the pair");
    const srp_credential* wider = relay::find_srp_credential(creds, "alice", 7);
    expect(wider != nullptr && wider->hashversion == 1,
           "a request above every entry falls back to the highest available");
    expect(relay::find_srp_credential(creds, "mallory", 1) == nullptr,
           "an unknown user is not found");
    expect(relay::find_srp_credential(creds, "alic", 1) == nullptr,
           "usernames match exactly, not by prefix");
  }
  {
    // A 3-field file (written before the hash-version field existed) must still
    // authenticate a peer that asks for the current version.
    std::vector<srp_credential> creds;
    srp_credential cred;
    expect(parse_srp_line("alice:YWJj:ZGVm", cred), "seed 3-field line");
    creds.push_back(cred);
    expect(relay::find_srp_credential(creds, "alice", 1) != nullptr,
           "a version-0-only file still serves a version-1 request");
    const srp_credential* fallback = relay::find_srp_credential(creds, "alice", 1);
    expect(fallback != nullptr && fallback->hashversion == 0,
           "and it reports the version it actually served");
  }

  std::printf("relay_srp_test: OK\n");
  return 0;
}
