# Third-party notices — open-broadcast-receiver

First-party code in this repository is licensed AGPL-3.0-or-later (see
`LICENSE`). The following third-party components are vendored in-tree or
linked at build/run time and retain their own licenses:

| Component | Where | License | Notes |
|-----------|-------|---------|-------|
| rist-cpp | `external/rist-cpp/` (vendored) | BSD-2-Clause | C++ wrapper around librist; see `external/rist-cpp/COPYING` |
| librist | fetched/built by rist-cpp (ExternalProject) | BSD-2-Clause | includes a local patch under `external/rist-cpp/patches/`, also BSD-2-Clause |
| cpp-httplib | `external/httplib.h` (vendored header) | MIT | HTTP control-plane server |
| nlohmann-json | system package | MIT | JSON parsing for the control plane |
| GStreamer | system package, dynamically linked | LGPL-2.1 | media pipelines; dynamic linking, no LGPL relink obligation triggered |

Keep the license files of vendored components intact when updating them.
When a new dependency is added, add it to this table in the same commit.
