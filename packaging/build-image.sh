#!/usr/bin/env bash
# Build the receiver container image from this checkout, tagged with the git
# commit's short SHA and (by default) :latest.
#
# The image is what the node agent runs per session as obr-<session_id> on the
# obr-sessions network; there is no long-running receiver stack.
#
# Usage (see docs/IMAGE.md):
#   packaging/build-image.sh                 # linux/amd64, tag <sha> + :latest
#   packaging/build-image.sh --pinned        # same, base pinned by digest
#   packaging/build-image.sh --platform linux/amd64,linux/arm64 --push
#
# Nothing is pushed unless --push is passed. A push needs a registry login that
# is done out-of-band (never here):
#   docker login ghcr.io -u <account>     # PAT with write:packages
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-ghcr.io/patcarter883/open-broadcast-receiver}"
PLATFORMS="${PLATFORMS:-linux/amd64}"
TAG="${TAG:-}"
BASE_IMAGE_LOCK="$REPO_ROOT/docker/base-image.lock"

pinned=0
latest=1
push=0

die() { printf 'build-image: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --tag)       TAG="${2:?--tag needs a value}"; shift 2 ;;
        --platform)  PLATFORMS="${2:?--platform needs a value}"; shift 2 ;;
        --pinned)    pinned=1; shift ;;
        --no-latest) latest=0; shift ;;
        --push)      push=1; shift ;;
        -h|--help)   sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           die "unknown argument: $1" ;;
    esac
done

command -v docker >/dev/null || die "docker not on PATH"
docker buildx version >/dev/null 2>&1 || die "docker buildx plugin not available"
[ -f "$REPO_ROOT/Dockerfile" ] || die "no Dockerfile at $REPO_ROOT"

if [ -z "$TAG" ]; then
    TAG="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null)" \
        || die "not a git checkout and --tag was not given"
fi
if ! git -C "$REPO_ROOT" diff --quiet 2>/dev/null; then
    printf 'build-image: WARNING working tree has uncommitted changes; the tag %s names the commit, not the tree\n' "$TAG" >&2
fi

# Multi-platform output cannot be loaded into the local image store, only pushed
# or written to an OCI layout, so a local multi-platform build is refused rather
# than silently producing nothing.
case "$PLATFORMS" in
    *,*) [ "$push" = 1 ] || die "multi-platform ($PLATFORMS) needs --push; single-platform local builds use --platform linux/amd64" ;;
esac

args=(--file "$REPO_ROOT/Dockerfile" --platform "$PLATFORMS" --tag "$IMAGE:$TAG")
[ "$latest" = 1 ] && args+=(--tag "$IMAGE:latest")

if [ "$pinned" = 1 ]; then
    [ -f "$BASE_IMAGE_LOCK" ] || die "--pinned needs $BASE_IMAGE_LOCK"
    base="$(tr -d '[:space:]' < "$BASE_IMAGE_LOCK")"
    [ -n "$base" ] || die "$BASE_IMAGE_LOCK is empty"
    args+=(--build-arg "BASE_IMAGE=$base")
    printf 'build-image: base pinned to %s\n' "$base"
fi

[ "$push" = 1 ] && args+=(--push)

printf 'build-image: image=%s tag=%s platform=%s push=%s\n' "$IMAGE" "$TAG" "$PLATFORMS" "$push"
docker buildx build "${args[@]}" "$REPO_ROOT"

if [ "$push" = 0 ] && [ "$PLATFORMS" = "linux/amd64" ]; then
    printf '\nbuild-image: %s\n' "$(docker image inspect --format '{{.Id}}' "$IMAGE:$TAG")"
    printf 'build-image: tags %s:%s %s:latest\n' "$IMAGE" "$TAG" "$IMAGE"
fi
