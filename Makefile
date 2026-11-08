# Container image build for the receiver.
#
# One entry point: packaging/build-image.sh, which tags the image with the git
# commit's short SHA plus :latest. See docs/IMAGE.md for the full contract.
#
#   make image            # local linux/amd64 image, tags <sha> and :latest
#   make image-pinned     # same, base image pinned to docker/base-image.lock
#   make image-multiarch  # amd64+arm64, pushed (needs a registry login)

IMAGE     ?= ghcr.io/patcarter883/open-broadcast-receiver
PLATFORMS ?= linux/amd64
TAG       ?=
BUILD     := packaging/build-image.sh
TAG_ARG   := $(if $(TAG),--tag $(TAG))

.PHONY: image image-pinned image-multiarch help

image:
	IMAGE=$(IMAGE) $(BUILD) $(TAG_ARG) --platform $(PLATFORMS)

image-pinned:
	IMAGE=$(IMAGE) $(BUILD) $(TAG_ARG) --platform $(PLATFORMS) --pinned

image-multiarch:
	IMAGE=$(IMAGE) $(BUILD) $(TAG_ARG) --platform linux/amd64,linux/arm64 --pinned --push

help:
	@sed -n '3,8p' Makefile
