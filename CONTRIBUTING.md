# Contributing to open-broadcast-receiver

Thanks for your interest in contributing.

## License and inbound policy

This project is licensed **AGPL-3.0-or-later** (see `LICENSE`). Contributions
are accepted under the same license — inbound = outbound. There is no CLA.

Every commit must carry a **Developer Certificate of Origin** sign-off
(<https://developercertificate.org/>):

```
git commit -s
```

which appends a `Signed-off-by: Your Name <you@example.com>` trailer,
certifying you have the right to submit the work under the project license.

> **Note on future policy:** for *substantial* contributions the project may
> in future introduce a contributor license agreement. Any such change will be
> announced in advance and will never apply retroactively to work already
> merged under the DCO.

## Practical notes

- Design decisions are recorded in `DECISIONS.md` (choice, rationale, rejected
  alternatives). Non-trivial design calls made during implementation get an
  entry in the same commit.
- Wire-visible changes update `docs/CONTRACT.md` in the same PR as the code.
- Keep clang-tidy passing (`.clang-tidy` at the repo root).
- Never commit compiled binaries — CI rejects ELF/PE/Mach-O files. Release
  artifacts ship via GitHub Releases with a SHA-256.
- Secrets (tokens, PSKs, stream keys) never enter URLs or logs. The redaction
  behaviour in the control plane is a contract property — extend it, never
  regress it.
