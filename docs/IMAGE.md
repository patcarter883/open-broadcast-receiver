# Container image

The receiver ships as one image built from the repo-root `Dockerfile`: a
`debian:sid` build stage (cmake + GStreamer headers, vendored librist compiled
`-DNDEBUG`) and a `debian:sid` runtime stage carrying the binary, GStreamer
plugins and the VAAPI userspace. It runs as uid `10001`, listens on
`8080/tcp` (control) and `5000/udp` (RIST), and the node agent starts one
container per session named `obr-<session_id>` on the `obr-sessions` network
with `/dev/dri` render nodes passed in.

Runtime packages are GStreamer (`good`/`bad`/`ugly`/`libav`) plus
`libva2`, `libva-drm2` and `mesa-va-drivers`. `mesa-va-drivers` is the
radeonsi (AMD) driver used by this deployment; Intel `iHD`/`i965` drivers are
deliberately absent.

## Build

```
make image
```

That is `packaging/build-image.sh`, which:

- builds from this checkout, using the repo `Dockerfile`;
- tags `<registry>/<image>:<short-sha>` and `<registry>/<image>:latest`;
- builds for `linux/amd64` only.

Equivalent direct invocation, with every knob visible:

```sh
packaging/build-image.sh --platform linux/amd64
# or
IMAGE=ghcr.io/patcarter883/open-broadcast-receiver TAG=local \
    packaging/build-image.sh --no-latest
```

`TAG`, `IMAGE` and `PLATFORMS` are also Make variables:

```sh
make image TAG=probe
make image IMAGE=obr-receiver
```

The short SHA is `git rev-parse --short HEAD` of the checkout the script runs
from. The script warns (and proceeds) when the tree has uncommitted changes,
because the tag then names the commit and not the tree.

## Pinned base image

`debian:sid` is a rolling tag, so an unpinned build can pick up a different
GStreamer or libva than the last one. `docker/base-image.lock` records one
digest of the base image, and `--pinned` feeds it to the Dockerfile's
`BASE_IMAGE` build arg:

```
make image-pinned
```

The `Dockerfile` default stays the floating `debian:sid` tag, so Komodo's build
and plain `docker build` are unchanged. Refresh the lock deliberately:

```sh
docker buildx imagetools inspect debian:sid --format '{{.Manifest.Digest}}'
```

## Multi-arch / x86-64 pinning

`--platform` is passed straight to `docker buildx build`, so the x86-64 image
is pinned explicitly by default (`linux/amd64`) rather than depending on the
builder's architecture. A multi-platform build writes a manifest list and
cannot be loaded locally, so the script refuses it without `--push`:

```sh
packaging/build-image.sh --platform linux/amd64,linux/arm64 --pinned --push
```

Only `linux/amd64` is exercised by this deployment; `arm64` is untested and the
VAAPI driver package set above is chosen for the AMD x86-64 nodes.

## Push

Nothing is pushed unless `--push` is given. Pushing needs a registry credential
that is supplied out of band and never written into the repo:

```sh
docker login ghcr.io -u patcarter883     # personal access token, write:packages
packaging/build-image.sh --pinned --push
```

Komodo's `obr-receiver-build` pushes on every build using the registry account
in `/etc/komodo/periphery.providers.toml`; the local path above is for building
and testing an image without going through Komodo.

## Running the built image locally

```sh
docker run --rm --network obr-sessions --name obr-<session_id> \
    --device /dev/dri:/dev/dri \
    -e OBR_TOKEN=... -p 8080:8080 -p 5000:5000/udp \
    ghcr.io/patcarter883/open-broadcast-receiver:latest
```

The `OBR_*` environment is mapped to CLI flags by `docker/entrypoint.sh`.

## Who needs registry auth

There are three separate consumers, and only one of them is Komodo:

| Consumer | Credential source | State |
| --- | --- | --- |
| `obr-receiver-build` (Komodo) | `/etc/komodo/periphery.providers.toml` → `[[docker_registry]]` account | works as-is; the build logs in itself and pushes on every build |
| Host `docker push` / `docker pull` from a shell | `~/.docker/config.json` | **empty by default** — `docker pull` returns `unauthorized` until someone runs `docker login ghcr.io` on that host |
| Session start on a node | the node agent's `RECEIVER_IMAGE`, pulled with the *host* docker login | tolerates a missing credential (see below) |

A Komodo build therefore does not need a host login: Periphery holds its own
registry account and pushes with it. The failure mode to watch for is the
opposite one — a host with no login that is also missing the image locally.

### Session start without registry auth

`node/session.go` pulls before running because `docker run <tag>` never
re-pulls a tag that is already present, so a node would otherwise serve
whatever receiver it first cached. The pull is deliberately non-fatal: if the
registry rejects the host (`unauthorized`) the agent logs it and runs the local
copy of that tag.

```
pull ghcr.io/patcarter883/open-broadcast-receiver:latest failed, using the local copy if present: ...
```

That makes a locally built tag a legitimate interim delivery path:

```sh
make image                      # -> <image>:<sha> + <image>:latest on this host
docker run ... ghcr.io/patcarter883/open-broadcast-receiver:latest
```

It only works while the tag is actually present (`docker images`), and it
silently pins the node to that copy — the drift is visible in the agent log and
nowhere else. Log the host in to ghcr when the build host is the node.

