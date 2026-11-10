// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for ingest direction (listen vs trunk/connect) and the URLs built
// from it. The rules asserted here are the ones that decide whether this
// process binds a socket at all, so they are pinned rather than left to the
// network lane: a connect-mode receiver that accidentally binds is a stream
// that never arrives while every health check says "listening".
// Framework-free; exit 0 = pass.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "lib/lib.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

auto contains(std::string_view haystack, std::string_view needle) -> bool
{
  return haystack.find(needle) != std::string_view::npos;
}

auto connect_config(std::string url) -> ingest_config
{
  ingest_config ing;
  ing.connect_url = std::move(url);
  return ing;
}

// validate_config takes a whole session config and mutates it, so the ingest
// block is spliced into a default one.
auto check(ingest_config ing) -> validation_result
{
  receiver_config cfg;
  cfg.ingest = std::move(ing);
  return validate_config(cfg);
}
}  // namespace

auto main() -> int
{
  // ---- listen mode is unchanged -------------------------------------------
  {
    ingest_config ing;  // defaults: rist://@[::]:5000, no connect_url
    expect(!ingest_is_connect_mode(ing), "default config is listen mode");
    expect(check(ing).ok,
           "default listen config validates");
    const std::string url = build_listener_url(ing);
    expect(url.starts_with("rist://@[::]:5000?"), "listener URL keeps the bind");
    expect(contains(url, "timing-mode=0"), "listener URL is SOURCE timing");
    expect(!contains(url, "profile="), "listener URL carries no profile=");
    expect(build_connect_url(ing).empty(), "no connect URL in listen mode");
  }

  // ---- connect mode: valid shapes -----------------------------------------
  {
    ingest_config ing = connect_config("rist://relay.example.net:6010");
    expect(ingest_is_connect_mode(ing), "a connect_url means connect mode");
    expect(check(ing).ok, "connect URL validates");

    const std::string url = build_connect_url(ing);
    // The whole point: no '@', so librist creates a Receiver Peer in connecting
    // mode instead of binding a listener.
    expect(contains(url, "rist://relay.example.net:6010?"),
           "connect URL keeps scheme+host:port");
    expect(!contains(url, "@"), "connect URL must not contain '@'");
    expect(contains(url, "timing-mode=0"),
           "connect URL is SOURCE timing (three-hop trunk)");
    expect(contains(url, "bandwidth="), "connect URL carries bandwidth");
    expect(contains(url, "buffer-min="), "connect URL carries buffer-min");
    expect(contains(url, "buffer-max="), "connect URL carries buffer-max");
    expect(contains(url, "rtt-min="), "connect URL carries rtt-min");
    expect(contains(url, "rtt-max="), "connect URL carries rtt-max");
    expect(contains(url, "reorder-buffer="), "connect URL carries reorder-buffer");
    expect(!contains(url, "profile="), "connect URL carries no profile=");
  }
  {
    // A caller-supplied query string is replaced, not appended: two
    // timing-mode= parameters is an ambiguous URL and librist takes the first.
    ingest_config ing = connect_config("rist://[2001:db8::1]:6010?timing-mode=1");
    expect(check(ing).ok, "IPv6 connect URL validates");
    const std::string url = build_connect_url(ing);
    expect(url.starts_with("rist://[2001:db8::1]:6010?"),
           "IPv6 base is preserved");
    expect(!contains(url, "timing-mode=1"), "caller timing-mode is replaced");
    expect(contains(url, "timing-mode=0"), "and the trunk keeps SOURCE");
  }
  {
    // rist6 scheme is accepted: the trunk may be v6-only.
    ingest_config ing = connect_config("rist6://relay.internal:6010");
    expect(check(ing).ok, "rist6 connect URL validates");
  }

  // ---- SRP client credentials ride the URL, in librist's names ------------
  {
    ingest_config ing = connect_config("rist://relay.example.net:6010");
    ing.srp_username = "venue";
    ing.srp_password = "hunter2";
    expect(check(ing).ok, "connect URL with an SRP pair validates");
    const std::string url = build_connect_url(ing);
    // librist reads exactly `username=` / `password=` (src/rist-common.c,
    // RIST_URL_PARAM_SRP_USERNAME/_PASSWORD); any other spelling is ignored and
    // the handshake fails with no diagnostic.
    expect(contains(url, "&username=venue"), "SRP username is param username=");
    expect(contains(url, "&password=hunter2"),
           "SRP password is param password=");
    expect(!contains(url, "srp-username"), "no invented srp-username param");
  }
  {
    ingest_config ing = connect_config("rist://relay.example.net:6010");
    ing.srp_username = "venue";  // half a pair
    const validation_result res = check(ing);
    expect(!res.ok, "half an SRP pair is refused");
    expect(res.field == "ingest.srp_username", "and names the field");
  }
  {
    // A credential must never be able to inject a RIST parameter: librist splits
    // on '&' and the first '=' with no unescaping, so anything outside the
    // unreserved set is percent-encoded on the way in.
    ingest_config ing = connect_config("rist://relay.example.net:6010");
    ing.srp_username = "ven&ue";
    ing.srp_password = "pw=x";
    expect(check(ing).ok, "special characters are accepted (encoded, not refused)");
    const std::string url = build_connect_url(ing);
    expect(contains(url, "&username=ven%26ue"),
           "an '&' in the username is percent-encoded");
    expect(contains(url, "&password=pw%3Dx"),
           "an '=' in the password is percent-encoded");
    // The injection that encoding prevents: a second parameter appearing raw.
    expect(!contains(url, "&ue"), "no raw '&' survives from the credential");
    expect(!contains(url, "=x&"), "no parameter is forged by the credential");
    const std::size_t params = std::count(url.begin(), url.end(), '&');
    // 7 tuning parameters + username + password = 8 separators, none from the
    // credential itself.
    expect(params == 8, "only the URL's own 8 '&' separators are present");
  }
  {
    ingest_config ing = connect_config("rist://relay.example.net:6010");
    ing.srp_username = "venue";
    ing.srp_password = "hunter2";
    expect(check(ing).ok, "an unreserved credential is passed through as-is");
    expect(contains(build_connect_url(ing), "&username=venue&password="),
           "plain credentials are not escaped unnecessarily");
  }
  {
    ingest_config ing;  // listen mode
    ing.srp_username = "venue";
    ing.srp_password = "pw";
    expect(!check(ing).ok,
           "SRP client credentials without a connect URL are refused");
  }

  // ---- connect mode: refused shapes ---------------------------------------
  {
    // '@' means "this end binds"; a connect URL with it is the exact
    // half-bound failure this mode exists to prevent.
    ingest_config ing = connect_config("rist://@:6010");
    const validation_result res = check(ing);
    expect(!res.ok, "connect URL with '@' is refused");
    expect(res.field == "ingest.connect_url", "and it names the field");
  }
  {
    ingest_config ing = connect_config("rist://relay.example.net:6010");
    ing.rist_listen = "rist://@[::]:5000";  // default: fine
    expect(check(ing).ok,
           "connect mode with the default listen URL validates");
    ing.rist_listen = "rist://@[::]:7000";  // explicit: a second leg
    const validation_result res = check(ing);
    expect(!res.ok, "connect mode plus an explicit listen URL is refused");
    expect(res.field == "ingest.connect_url", "and it names connect_url");
  }
  {
    ingest_config ing = connect_config("relay.example.net:6010");
    expect(!check(ing).ok,
           "a connect URL without a scheme is refused");
  }
  {
    ingest_config ing = connect_config("udp://relay.example.net:6010");
    expect(!check(ing).ok,
           "a non-RIST scheme is refused");
  }
  {
    ingest_config ing = connect_config("rist://relay.example.net");
    expect(!check(ing).ok,
           "a connect URL with no port is refused (nothing to dial)");
  }
  {
    ingest_config ing = connect_config("rist://:6010");
    expect(!check(ing).ok,
           "a connect URL with no host is refused");
  }

  // ---- tuning is still validated in connect mode --------------------------
  {
    ingest_config ing = connect_config("rist://relay.example.net:6010");
    ing.bandwidth = 10;  // below the 100 floor
    const validation_result res = check(ing);
    expect(!res.ok, "connect mode still validates bandwidth");
    expect(res.field == "ingest.bandwidth", "and names the tuning field");
  }

  std::printf("connect_url_test: OK\n");
  return 0;
}
