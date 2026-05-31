# IMPL_PLAN.md — implementation plan (both repos)

Date: 2026-05-30
Scope: build the headless `open-broadcast-receiver` from scratch and add a receiver-control client + UI section to the
existing `open-broadcast-encoder`. Wire format is CONTRACT.md; pipeline strings are GSTREAMER.md; rationale is
DECISIONS.md. Paths are absolute.

---

## PART A — Receiver repo file tree (`/home/pat/Projects/open-broadcast/open-broadcast-receiver`)

Empty git repo, branch `main`, no commits. Mirror the encoder's modular layout minus FLTK/NDI.

```
open-broadcast-receiver/
├── CMakeLists.txt
├── .gitignore
├── README.md
├── DECISIONS.md                      # (this redevelopment's ADR; see DECISIONS.md doc)
├── cmake/
│   ├── prelude.cmake                 # verbatim from encoder (in-source build guard)
│   ├── variables.cmake               # encoder's, project name substituted
│   ├── ExternalBuilds.cmake          # rist-cpp block ONLY (drop FLTK + sdp-tools-cpp)
│   └── modules/
│       └── FindGStreamer.cmake       # OPTIONAL — only if a module needs the GSTREAMER_INCLUDE_DIRS var
├── external/
│   ├── rist-cpp/                      # submodule or copy of encoder's external/rist-cpp (PATCHED sendOOBData)
│   └── httplib.h                     # verbatim copy of /mnt/data/projects/llama.cpp/vendor/cpp-httplib/httplib.h (0.40.0)
└── source/
    ├── main.cpp                      # headless entry: gst_init, args, wire handlers, signal-wait
    ├── lib/
    │   ├── CMakeLists.txt
    │   ├── lib.h                      # shared types + wan_telemetry (byte-identical) + config structs
    │   └── lib.cpp                    # JSON to/from + build_listener_url
    ├── receive/
    │   ├── CMakeLists.txt
    │   ├── receive.h                  # class rist_receive (RAII RISTNetReceiver)
    │   └── receive.cpp                # callbacks, OOB telemetry send, ADVANCED profile
    ├── restream/
    │   ├── CMakeLists.txt
    │   ├── restream.h                 # class restream (GStreamer pipeline manager)
    │   └── restream.cpp               # copy/reencode/multi-output assembly (GSTREAMER.md)
    └── control/
        ├── CMakeLists.txt
        ├── control.h                  # class control_server (httplib::Server)
        └── control.cpp                # REST routes + auth + JSON marshalling (CONTRACT.md)
```

### A.1 Top-level CMakeLists.txt (key contents)
```cmake
cmake_minimum_required(VERSION 3.28)
include(cmake/prelude.cmake)
project(open-broadcast-receiver VERSION 0.1.0 LANGUAGES C CXX)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(cmake/variables.cmake)
include(cmake/ExternalBuilds.cmake)              # rist-cpp only -> IMPORTED rist, ristnet; sets RIST_INSTALL_DIR
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.2 REQUIRED)         # system 3.12.0
find_package(PkgConfig REQUIRED)
  pkg_search_module(gstreamer     REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.28)
  pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.28)
  pkg_search_module(gstreamer-sdp REQUIRED IMPORTED_TARGET gstreamer-sdp-1.0>=1.28)
  pkg_search_module(gstreamer-rtp REQUIRED IMPORTED_TARGET gstreamer-rtp-1.0>=1.28)
  pkg_search_module(gstreamer-video REQUIRED IMPORTED_TARGET gstreamer-video-1.0>=1.28)
add_subdirectory(source/lib)
add_subdirectory(source/receive)
add_subdirectory(source/restream)
add_subdirectory(source/control)
add_executable(open-broadcast-receiver_exe source/main.cpp)
add_executable(open-broadcast-receiver::exe ALIAS open-broadcast-receiver_exe)
set_property(TARGET open-broadcast-receiver_exe PROPERTY OUTPUT_NAME open-broadcast-receiver)
target_compile_features(open-broadcast-receiver_exe PRIVATE cxx_std_20)
target_link_libraries(open-broadcast-receiver_exe PRIVATE
  receiver_lib receive_lib restream_lib control_lib
  ristnet
  PkgConfig::gstreamer PkgConfig::gstreamer-app PkgConfig::gstreamer-sdp
  PkgConfig::gstreamer-rtp PkgConfig::gstreamer-video
  Threads::Threads nlohmann_json::nlohmann_json)
target_include_directories(open-broadcast-receiver_exe PUBLIC SYSTEM
  ${RIST_INSTALL_DIR}/include ${RIST_INSTALL_DIR}/include/rist-cpp ${GSTREAMER_INCLUDE_DIRS})
target_include_directories(open-broadcast-receiver_exe PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/external)
# NO find_package(NDI); NO external_fltk.
```

### A.2 cmake/ExternalBuilds.cmake
Copy ONLY the encoder's rist-cpp block (the `external_rist_cpp` ExternalProject + IMPORTED `rist`/`ristnet` targets,
the lines that set `RIST_INSTALL_DIR`, `IMPORTED_LOCATION` librist.a/libristnet.a, `INTERFACE_INCLUDE_DIRECTORIES`
include + include/librist (rist) and include + include/rist-cpp (ristnet), and ristnet's
`INTERFACE_LINK_LIBRARIES "rist;Threads::Threads"`). Prepend `include(ExternalProject)`,
`set(EXTERNAL_BUILD_DIR "${PROJECT_SOURCE_DIR}/build-external")`, `find_package(Threads REQUIRED)`. **Drop the FLTK and
sdp-tools-cpp blocks entirely.** `SOURCE_DIR` must point at `${PROJECT_SOURCE_DIR}/external/rist-cpp`.

### A.3 source/lib (receiver_lib STATIC)
- **CMakeLists.txt:** `add_library(receiver_lib STATIC)`; sources `lib.h lib.cpp`; `target_include_directories PUBLIC
  ${CMAKE_SOURCE_DIR}/source`; `cxx_std_20`; `target_link_libraries PUBLIC nlohmann_json::nlohmann_json`.
- **lib.h** (`#pragma once`, no FLTK/NDI/json includes in the header where avoidable — keep nlohmann out of widely
  included headers; put JSON conversion declarations only):
  - `enum class codec : uint8_t { h264, h265, av1 };` and
    `enum class encoder : uint8_t { amd, qsv, nvenc, software };` — **byte-for-byte the encoder's order** (the JSON int
    mapping depends on it).
  - `enum class output_proto : uint8_t { rtmp, srt, rist, udp };` (rtmps maps to rtmp branch by URL scheme).
  - `struct __attribute__((packed)) wan_telemetry { uint8_t link_quality; uint32_t worst_case_rtt; };
    static_assert(sizeof(wan_telemetry)==5, ...);` — identical to encoder lib.h:53-59.
  - `struct video_disposition { bool reencode=false; codec out_codec=codec::h264; encoder enc=encoder::software;
    int bitrate_kbps=4300; bool upscale=false; int width=2560; int height=1440; };`
  - `struct audio_disposition { bool reencode=false; int bitrate_kbps=128; };`
  - `struct destination { std::string id; output_proto proto=output_proto::rtmp; std::string url;
    std::string key_or_streamid; int latency_ms=200; int sender_buffer=0; std::string cname;
    video_disposition video; audio_disposition audio; };`
  - `struct ingest_config { std::string rist_listen="rist://@[::]:5000"; int bandwidth=6000, buffer_min=245,
    buffer_max=5000, rtt_min=40, rtt_max=500, reorder_buffer=240; };`
  - `struct receiver_config { int schema_version=1; std::string session_id; ingest_config ingest; codec in_codec=codec::h264;
    std::vector<destination> destinations; };`
  - `struct receiver_state { std::atomic_bool is_running{false}; std::mutex mutex; receiver_config cfg;
    std::string session_id; std::chrono::steady_clock::time_point started_at;
    std::atomic<int> link_quality{0}; std::atomic<uint32_t> worst_rtt{0}; std::string last_bus_error; };`
  - `struct app_context { receiver_state state; std::unique_ptr<class rist_receive> receive;
    std::unique_ptr<class restream> restreamer; std::unique_ptr<class control_server> control;
    std::string auth_token; };`
  - Declarations: `void to_json(json&, const receiver_config&)` etc. (defined in lib.cpp);
    `std::string build_listener_url(const ingest_config&, codec);`
- **lib.cpp:** implement `to_json`/`from_json` for `receiver_config`/`destination`/`video_disposition`/
  `audio_disposition`/`ingest_config` using `nlohmann::json`, mapping codec/encoder/proto **strings**↔enums
  (CONTRACT §3) and applying defaults via `j.value("key", default)`. Implement `build_listener_url`:
  `rist://@[::]:{port}?bandwidth={bw}&buffer-min={bmin}&buffer-max={bmax}&rtt-min={rmin}&rtt-max={rmax}&reorder-buffer={ro}&timing-mode=2&profile=2`
  — **note the `&` after `bandwidth={bw}`** (fixes the original ndi-rist-server missing-`&` bug). Port is parsed from
  `ingest.rist_listen`.

### A.4 source/receive (receive_lib STATIC)
- **CMakeLists.txt:** `add_library(receive_lib STATIC)`; `target_link_libraries PUBLIC ristnet PRIVATE receiver_lib
  restream_lib PkgConfig::gstreamer PkgConfig::gstreamer-app`; SYSTEM include `${RIST_INSTALL_DIR}/include` +
  `/include/rist-cpp` + `${GSTREAMER_INCLUDE_DIRS}`.
- **receive.h:** `class rist_receive` owning `std::unique_ptr<RISTNetReceiver>`; deleted copy/move;
  `using push_fn = std::function<int(const uint8_t*, size_t)>;` `void start(const receiver_config&, push_fn);`
  `void stop();` `void set_log_callback(int(*)(void*, rist_log_level, const char*));`
  `void update_state(receiver_state&);` private: `push_fn mPush; std::atomic<rist_peer*> mPeer{nullptr};
  std::recursive_mutex/atomics for state`.
- **receive.cpp:**
  - Settings: `s.mProfile = RIST_PROFILE_ADVANCED` (**load-bearing**; reference used MAIN);
    `s.mLogLevel = RIST_LOG_INFO`; populate `s.mPeerConfig.recovery_length_min/max` from buffer_min/max,
    `recovery_rtt_min/max` from rtt_min/max, `recovery_maxbitrate` from bandwidth*… Leave `mPSK`/`mCNAME` empty.
  - URLs vector = `{ build_listener_url(cfg.ingest, cfg.in_codec) }` (one per stream if multi-stream).
  - `validateConnectionCallback`: accept (return a NetworkConnection); optionally stash flow context in `mObject`.
  - `networkDataCallback`: capture `pPeer` into `mPeer` on first packet (atomic), `return mPush(buf, len);`. Return 0
    (keep connection) on transient gst errors; never return -1 except on genuine fatal.
  - `statisticsCallback`: guard `stats_type == RIST_STATS_RECEIVER_FLOW`; read `receiver_flow.quality` (clamp/round →
    `link_quality`) and `receiver_flow.rtt` (→ `worst_rtt`, optionally max over `peers[].rtt`); store into
    `receiver_state` atomics for `/status`; build the 5-byte packet (`htonl` rtt) and `sendOOBData(mPeer, pkt, 5)` if
    `mPeer != nullptr`.
  - `clientDisconnectedCallback`: clear `mPeer` (compare to stored), clear telemetry.
  - Teardown (`stop`): `closeAllClientConnections(); destroyReceiver();` then clear callbacks (encoder transport.cpp
    order). Guard `mPush`/`mPeer` for the librist callback threads.
  - **The vendored `RISTNetReceiver::sendOOBData` must be patched** to log+return false on `rist_oob_write` failure
    rather than `destroyReceiver()` (DECISIONS §6).

### A.5 source/restream (restream_lib STATIC)
- **CMakeLists.txt:** `add_library(restream_lib STATIC)`; `target_link_libraries PRIVATE receiver_lib
  PkgConfig::gstreamer PkgConfig::gstreamer-app PkgConfig::gstreamer-video`.
- **restream.h:** `class restream { restream(std::function<void(const std::string&)> log); ~restream();
  bool start(const receiver_config&, std::string& err); void stop(); int push_buffer(const uint8_t*, size_t);
  void update_state(receiver_state&); }`; private `build_pipeline_string(const receiver_config&)` +
  `pipeline_build_source/_video/_audio/_outputs`, `GstElement* pipeline; GstElement* appsrc; GstBus* bus;
  std::mutex pipeline_mutex; std::atomic_bool running, cleaned_up; std::thread bus_thread;`.
- **restream.cpp:** assemble the single `gst_parse_launch` string per GSTREAMER.md:
  - SOURCE: appsrc idiom + `queue2 ! tsparse set-timestamps=true alignment=7 ! tsdemux name=demux`; after parse,
    `gst_bin_get_by_name(..., "videosrc")` and `gst_app_src_set_caps(video/mpegts,systemstream=true,packetsize=188)`.
  - VIDEO: copy (§2) or reencode (§4) via a `[encoder][codec]` template table reusing the encoder's 12 fragment
    strings (encode.cpp:197-253), ending `tee name=vtee`. Insert upscale (§6) when `video.upscale`.
  - AUDIO: copy (§3) or reencode (§5), ending `tee name=atee`.
  - OUTPUTS: per destination, append the §7 template with unique `{N}` and interpolated (validated) URL/key/params.
    Enforce the RTMP codec constraint (§9 / CONTRACT §4) **before** launch.
  - `push_buffer`: `gst_buffer_new_memdup` then `gst_app_src_push_buffer` (consumes the ref — do NOT unref); return 0
    on FLUSHING/EOS to keep the RIST connection.
  - Bus watch thread captures ERROR/EOS into `receiver_state.last_bus_error`.
  - `clear_pipeline_state()` under `pipeline_mutex`: set NULL, unref in reverse (encode.cpp discipline).

### A.6 source/control (control_lib STATIC)
- **CMakeLists.txt:** `add_library(control_lib STATIC)`; `target_include_directories PUBLIC ${CMAKE_SOURCE_DIR}/source
  ${CMAKE_SOURCE_DIR}/external`; `target_link_libraries PUBLIC nlohmann_json::nlohmann_json PRIVATE receiver_lib
  Threads::Threads`.
- **control.h:** `class control_server { using start_fn=std::function<bool(const receiver_config&, std::string& err,
  int& http_status)>; using stop_fn=std::function<bool(const std::string& session_id, std::string& err,
  int& http_status)>; using status_fn=std::function<nlohmann::json()>; control_server(app_context&);
  ~control_server(); void set_handlers(start_fn,stop_fn,status_fn); void listen(const std::string& host, int port);
  void stop_listening(); private: httplib::Server mSrv; std::thread mThread; app_context& mCtx; bool authorized(const
  httplib::Request&) const; };`
- **control.cpp:**
  - `set_payload_max_length(256*1024); set_read_timeout(5,0); set_write_timeout(5,0); set_keep_alive_timeout(5);`
  - `set_pre_routing_handler`: constant-time compare of `Authorization: Bearer <token>` vs `mCtx.auth_token`; on
    mismatch set 401 `{"ok":false,"error_code":"unauthorized"}` and return `HandlerResponse::Handled`. Skip the
    compare only if `auth_token` empty (dev mode).
  - `POST /start`: `json::parse(req.body)` (catch → 400 invalid_schema) → `from_json` → `receiver_config`; validate
    schema_version==1 and all CONTRACT §4 cross-field rules; call `start_fn(cfg, err, status)`; respond per CONTRACT
    (200/400/409/500). Redact `key_or_streamid` in logs.
  - `POST /stop`: parse optional session_id; `stop_fn`; 200 or 409 session_mismatch.
  - `GET /status`: `status_fn()` serialized (telemetry from `receiver_state` atomics; redact keys).
  - `GET /healthz`: 200 `{"ok":true}` (still token-gated via pre-routing).
  - `listen` runs `mSrv.listen(host, port)` on `mThread`. `stop_listening`/dtor: `mSrv.stop(); join();`.

### A.7 source/main.cpp
`gst_init`; parse `--control-port` (default 8080), `--rist-port` (default 5000), `--token` (default empty=dev),
`--help`. Construct `app_context ctx; ctx.auth_token=token; ctx.receive=make_unique<rist_receive>();
ctx.restreamer=make_unique<restream>(stderr_log);`. `control_server control(ctx); control.set_handlers(...)`:
- `start_fn(cfg, err, status)`: lock `ctx.state.mutex`; if running and body/session differs → set status=409,
  err="already_running", return false; idempotent-identical → status=200 return true; else
  `restreamer->start(cfg, err)` then `receive->start(cfg, [r=ctx.restreamer.get()](const uint8_t* b, size_t n){ return
  r->push_buffer(b,n); });` set `is_running`; on gst failure status=500.
- `stop_fn(session_id, err, status)`: validate session; `receive->stop(); restreamer->stop();` clear `is_running`.
- `status_fn`: build the CONTRACT §6 JSON from `receiver_state`.
Then `control.listen("0.0.0.0", control_port);` install SIGINT/SIGTERM handler (sigwait / condition var); block until
signalled; then `control.stop_listening(); stop_fn(...);` teardown in reverse (control, receive, restream). Idle until
`POST /start`. No FLTK, no NDI.

### A.8 .gitignore / README.md
`.gitignore`: `/build/`, `/build-external/`, `.cache/`, `compile_commands.json`. README: build
(`cmake -S . -B build -D CMAKE_BUILD_TYPE=Release; cmake --build build`), run
(`./build/open-broadcast-receiver --control-port 8080 --rist-port 5000 --token SECRET`), and a pointer to CONTRACT.md
for the REST API + the codec/encoder string↔int mapping; note ADVANCED profile + 5-byte OOB telemetry.

---

## PART B — Encoder change-list (`/home/pat/Projects/open-broadcast/open-broadcast-encoder`)

### B.1 New files

| Path | Purpose |
|------|---------|
| `source/control/control.h` | `class control_client`: thin httplib client. `control_client(host,port,token)`; `bool start(const receiver_control_config&, std::string& err)`; `bool stop(const std::string& session_id, std::string& err)`; `std::optional<nlohmann::json> poll_status()`. unique_ptr member, deleted copy/move (transport.h style). |
| `source/control/control.cpp` | Implements with `httplib::Client`: build the CONTRACT §4 JSON body from `receiver_control_config` (ingest/source/outputs), `POST /start`, `POST /stop`, set `Authorization: Bearer <token>`; map response → bool + error string. |
| `source/control/CMakeLists.txt` | `add_library(control_lib STATIC)` mirroring transport/CMakeLists.txt; `target_include_directories PUBLIC ${CMAKE_SOURCE_DIR}/source ${CMAKE_CURRENT_LIST_DIR}/../../external`; link `nlohmann_json::nlohmann_json open-broadcast-encoder_lib Threads::Threads`; `cxx_std_20`. |
| `external/httplib.h` | Verbatim copy of `/mnt/data/projects/llama.cpp/vendor/cpp-httplib/httplib.h` (0.40.0). Same header the receiver vendors; keep byte-identical. |

### B.2 Modified files

| Path | Change |
|------|--------|
| `source/lib/lib.h` | Add `enum class output_proto { rtmp, srt, rist, udp };`; `struct reencode_config { bool enabled=false; encoder enc=encoder::software; codec out_codec=codec::h264; int bitrate=8000; bool upscale=false; int width=2560; int height=1440; };`; `struct destination { output_proto proto=output_proto::rtmp; std::string url; std::string stream_key; bool reencode=false; reencode_config video; };`; `struct receiver_control_config { std::string control_host="127.0.0.1"; int control_port=8080; std::string token; std::string session_id; codec in_codec=codec::h264; std::vector<destination> destinations; };`. Add a `receiver_control_config receiver_ctl;` member to `struct library` (next to `output_cfg`). **Do NOT renumber `codec`/`encoder`** (the receiver maps the ints 1:1). **Do NOT** add a nlohmann include to lib.h — keep JSON in control.cpp. Keep `wan_telemetry` unchanged. |
| `source/main.cpp` | Add `std::unique_ptr<control_client>` to `app_context` (or hold in `ctx.lib`). In `run_transport()`/the start path, BEFORE the encoder begins sending RIST, construct `control_client` from `ctx.lib.receiver_ctl` and `POST /start` to provision the receiver; log via `transport_log`. In `stop()`, after tearing down the local encoder/transport, `control_client->stop()` so the receiver releases its pipeline. Add `apply_receiver_control()` wiring fn passed to `init_ui_callbacks` (analogous to `run_transport`/`scaling_source_changed`). `#include "control/control.h"`. Leave `rist_oob_cb` unchanged (already consumes the receiver's `wan_telemetry`). |
| `source/ui/ui.h` | Add widgets/callbacks for the Output/Receiver-control section: `Fl_Input* input_control_host; Fl_Input* input_control_port; Fl_Input* input_control_token;` plus a v1 single-destination block: `Fl_Choice* choice_dest_proto; Fl_Input* input_dest_url; Fl_Input* input_dest_key; Fl_Check_Button* check_reencode; Fl_Input* input_reencode_bitrate; Fl_Choice* choice_reencode_encoder; Fl_Choice* choice_reencode_codec; Fl_Check_Button* check_upscale;`. Add `static Fl_Menu_Item menu_choice_dest_proto[]` and reuse codec/encoder menus. Extend `init_ui_callbacks` to accept `receiver_control_config*` + an `apply_receiver_control` FuncPtr; add private cb method decls. `#include <FL/Fl_Check_Button.H>`. |
| `source/ui/ui.cpp` | Build the widgets inside the existing Output `Fl_Flex` (currently `input_rist_address` + Start/Stop, ~lines 280-306): rows for Control Host/Port/Token, then a destination block (proto/url/key/reencode+bitrate+encoder+codec+upscale). Define `menu_choice_dest_proto[]` (RTMP/SRT/RIST/UDP → user_data ints matching `output_proto`). Implement callbacks writing into `receiver_control_config`. Bind via `FL_METHOD_CALLBACK_*`; call `apply_receiver_control` on Start. Keep `input_rist_address` (the encoder's RIST OUT target); recommend auto-deriving the receiver RIST listener host/port from it. FLTK thread-safety unchanged (these run on the FLTK thread). |
| `CMakeLists.txt` | `add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/source/control)`; `find_package(nlohmann_json 3.2 REQUIRED)`; add `control_lib` to `open-broadcast-encoder_exe` link libs; add `${CMAKE_CURRENT_LIST_DIR}/external` to the exe PUBLIC include dirs (vendored httplib.h) and `${CMAKE_CURRENT_LIST_DIR}/source/control` to the include list. |
| `source/ui/CMakeLists.txt` | No new link needed (ui only edits config + calls a FuncPtr, never touches `control_client`). Only add `nlohmann_json::nlohmann_json PRIVATE` if json types leak into ui (they should not). |

---

## PART C — Dependency wiring (both repos)

- **cpp-httplib (vendored, header-only):** copy `/mnt/data/projects/llama.cpp/vendor/cpp-httplib/httplib.h` →
  `open-broadcast-receiver/external/httplib.h` and `open-broadcast-encoder/external/httplib.h`. Add `<repo>/external`
  to the include dirs of the control module/exe; `#include "httplib.h"`. Needs only pthread (via `Threads::Threads`,
  already linked in both repos). Plain HTTP → no OpenSSL define for v1. Pin version 0.40.0; keep the two copies
  byte-identical.
- **nlohmann/json (system):** `find_package(nlohmann_json 3.2 REQUIRED)` in both top-level CMakeLists; link
  `nlohmann_json::nlohmann_json` on receiver `receiver_lib`+`control_lib` and encoder `control_lib`. Do not vendor.
- **rist-cpp:** receiver copies the encoder's `external_rist_cpp` ExternalProject block (IMPORTED `rist`/`ristnet`).
  Provision `external/rist-cpp` as a git submodule of the same upstream the encoder uses, OR copy the encoder's tree;
  **apply the `sendOOBData` patch** (DECISIONS §6) in the receiver's copy.
- **GStreamer:** `pkg_search_module(... IMPORTED_TARGET ...)`, link `PkgConfig::gstreamer*`. `gstreamer-app` is
  mandatory (appsrc push).

---

## PART D — Build / verify order

1. Receiver: provision `external/rist-cpp` + `external/httplib.h`, then `cmake -S . -B build -D CMAKE_BUILD_TYPE=Release;
   cmake --build build`. rist-cpp builds once into `build-external/`.
2. Locally testable (this box): copy paths, software h264/h265 reencode, all three output protocols. nvenc/amf/qsv +
   rav1enc fragments compile into strings but parse-fail only on a box lacking the element — gate via registry lookup
   at start and return `encoder_unavailable`.
3. Encoder: vendor `external/httplib.h`, apply the lib.h/control/ui/CMake changes, rebuild.
4. End-to-end loopback: run receiver with `--token SECRET`; `curl` `POST /start` / `GET /status` / `POST /stop` with
   the bearer token to exercise the control plane independently of the encoder; then point the encoder's
   receiver-control UI at `127.0.0.1:8080` and confirm RIST media + 5-byte OOB telemetry round-trips (encoder
   `wan_quality`/`wan_rtt` populate).
5. Per CLAUDE.md, run an ASan-clean build of the new control header usage and the receiver before relying on it.
