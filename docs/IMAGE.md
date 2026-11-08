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

## Address family

The image ships `/etc/gai.conf` with:

```
precedence ::ffff:0:0/96 100
```

Node networks carry no IPv6 route, so every `getaddrinfo()` caller in the image
must rank IPv4 above IPv6 for a name that answers both families. Without the
policy, `GResolver` (what `rtmp2sink`'s `GSocketClient` uses) returns the IPv6
answer first whenever the container has any IPv6 address configured, and the
publish attempts an unroutable address. The file is a normal image layer, so it
survives container start; no per-run DNS flags or `--add-host` pins are needed,
and no destination address is hardcoded.

Verify from a container built from this image:

```sh
docker run --rm --entrypoint bash obr-receiver:local -c \
  'getent ahosts a.rtmp.youtube.com'
```

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
```

The one command that builds **and publishes** the image:

```sh
make image-push
```

It runs `packaging/build-image.sh --platform linux/amd64 --pinned --push`,
which tags `<registry>/<image>:<short-sha>` and `:latest` and pushes both. The
base is pinned to `docker/base-image.lock`, so re-running it for the same commit
resolves the same Debian packages instead of whatever `debian:sid` happens to be
that morning — that is what makes the published artifact reproducible rather
than merely repeatable.

Verify the push actually landed in the registry (not just in the local daemon):

```sh
docker buildx imagetools inspect \
  ghcr.io/patcarter883/open-broadcast-receiver:$(git rev-parse --short HEAD)
```

The GHCR package is **private**, so every form of inspection and pull needs the
host login above; an unauthenticated `docker manifest inspect` returns
`unauthorized`, not a manifest.

`make image-push` always tags `:latest` as well, which repoints what a session
pulls. To publish a probe or throwaway tag without touching `:latest`:

```sh
make image-push TAG=probe-$(date +%Y%m%d) LATEST=0
```

Equivalent direct invocation:

```sh
packaging/build-image.sh --pinned --push
```

Komodo's `obr-receiver-build` pushes on every build using the registry account
in `/etc/komodo/periphery.providers.toml` and tags the same way (`<sha>`,
`<version>`, `<major>.<minor>`, `<major>`, `latest`), so `make image-push` and a
Komodo build of the same commit produce the same tag set. The local path exists
for publishing without going through Komodo.

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

