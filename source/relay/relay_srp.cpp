// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "relay/relay_srp.h"

#include <array>
#include <cctype>
#include <fstream>
#include <optional>

namespace relay
{
namespace
{
constexpr std::string_view k_b64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

auto b64_value(char chr) -> int
{
  const auto pos = k_b64_alphabet.find(chr);
  return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

auto split_fields(std::string_view line,
                  std::array<std::string_view, 5>& out) -> std::size_t
{
  std::size_t count = 0;
  std::size_t start = 0;
  while (count < out.size()) {
    const std::size_t sep = line.find(':', start);
    if (sep == std::string_view::npos) {
      out[count++] = line.substr(start);
      return count;
    }
    out[count++] = line.substr(start, sep - start);
    start = sep + 1;
  }
  return count;
}

auto parse_hashversion(std::string_view field) -> int
{
  int value = 0;
  bool any = false;
  for (const char chr : field) {
    if (!std::isdigit(static_cast<unsigned char>(chr))) {
      break;
    }
    any = true;
    value = value * 10 + (chr - '0');
    if (value > 99) {
      return 99;
    }
  }
  return any ? value : 0;
}
}  // namespace

auto base64_decode(std::string_view in, std::vector<std::uint8_t>& out) -> bool
{
  out.clear();
  std::uint32_t buffer = 0;
  int bits = 0;
  bool saw_padding = false;
  for (const char chr : in) {
    if (chr == '=') {
      // Padding only ever trails. Anything after it is malformed.
      saw_padding = true;
      continue;
    }
    if (saw_padding) {
      return false;
    }
    if (std::isspace(static_cast<unsigned char>(chr)) != 0) {
      return false;
    }
    const int value = b64_value(chr);
    if (value < 0) {
      return false;
    }
    buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFU));
    }
  }
  // Leftover bits must be zero padding of an incomplete group, never data.
  return bits < 6;
}

auto parse_srp_line(std::string_view line, srp_credential& out) -> bool
{
  // Tolerate CRLF files written on another host.
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.remove_suffix(1);
  }
  if (line.empty() || line.front() == '#') {
    return false;
  }
  std::array<std::string_view, 5> fields {};
  const std::size_t count = split_fields(line, fields);
  if (count < 3 || fields[0].empty() || fields[1].empty() || fields[2].empty()) {
    return false;
  }
  srp_credential parsed;
  parsed.username = std::string(fields[0]);
  if (!base64_decode(fields[1], parsed.verifier)
      || !base64_decode(fields[2], parsed.salt) || parsed.verifier.empty()
      || parsed.salt.empty())
  {
    return false;
  }
  // Field 4 is a constant ("3") in every file ristsrppasswd writes — it is the
  // line FORMAT version, not a credential property, so it is ignored. The
  // credential's hash version is field 5: `correct ? 1 : 0`, i.e. 0 = librist's
  // pre-0.2.16 legacy combined hash, 1 = the RFC 5054-correct hash that every
  // current peer computes. A file without field 5 predates the flag and is read
  // as legacy (0); a file without field 4 at all is read as 0 too.
  if (count >= 5) {
    parsed.hashversion = parse_hashversion(fields[4]) == 0 ? 0 : 1;
  } else {
    parsed.hashversion = 0;
  }
  out = std::move(parsed);
  return true;
}

auto load_srp_file(const std::string& path) -> std::vector<srp_credential>
{
  std::vector<srp_credential> creds;
  std::ifstream file(path);
  if (!file) {
    return creds;
  }
  std::string line;
  while (std::getline(file, line)) {
    srp_credential cred;
    if (parse_srp_line(line, cred)) {
      creds.push_back(std::move(cred));
    }
  }
  return creds;
}

auto find_srp_credential(const std::vector<srp_credential>& creds,
                         std::string_view username,
                         int wanted_hashversion) -> const srp_credential*
{
  // `hashversion` is IN/OUT for librist: "the maximum supported hashversion
  // that should be looked up" in, "the hashversion of the found pair" out, and
  // librist then sets eapversion3 from what comes back. So the answer must be
  // the entry computed with the SAME hash the peer used: exact match first,
  // else the newest below the request, else the oldest above it.
  //
  // Getting this wrong is not a tuning error, it is a failed handshake: a
  // ristsrppasswd file holds both variants for one user (legacy 0 and correct
  // 1) and their verifiers are computed differently from the same password.
  const srp_credential* exact = nullptr;
  const srp_credential* highest_below = nullptr;
  const srp_credential* lowest_above = nullptr;
  for (const srp_credential& cred : creds) {
    if (cred.username != username) {
      continue;
    }
    if (cred.hashversion == wanted_hashversion) {
      if (exact == nullptr) {
        exact = &cred;
      }
    } else if (cred.hashversion < wanted_hashversion) {
      if (highest_below == nullptr
          || cred.hashversion > highest_below->hashversion)
      {
        highest_below = &cred;
      }
    } else if (lowest_above == nullptr
               || cred.hashversion < lowest_above->hashversion)
    {
      lowest_above = &cred;
    }
  }
  if (exact != nullptr) {
    return exact;
  }
  return highest_below != nullptr ? highest_below : lowest_above;
}

}  // namespace relay
