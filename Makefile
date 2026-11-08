# Container image build and decode-fixture targets for the receiver.
#
# Image: packaging/build-image.sh tags the image with the git commit's short
# SHA plus :latest. See docs/IMAGE.md for the full contract.
#
#   make image            # local linux/amd64 image, tags <sha> and :latest
#   make image-pinned     # same, base image pinned to docker/base-image.lock
#   make image-push       # THE publish command: pinned base, builds and pushes
#                         #   <sha> (and :latest) to the registry
#   make image-push TAG=x LATEST=0   # push only tag x, leave :latest alone
#   make image-multiarch  # amd64+arm64, pushed (needs a registry login)
#
# AV1-in-MPEG-TS fixture (de-risks end-to-end integration; see
# test/tools/make-av1-ts-fixture.sh):
#
#   make fixture          # build/av1-fixture.ts with this machine's AV1 encoder
#   make fixture-decode   # build the fixture, then run the receiver's own
#                         # decode stage over it and print the frame counters

IMAGE     ?= ghcr.io/patcarter883/open-broadcast-receiver
PLATFORMS ?= linux/amd64
TAG       ?=
LATEST    ?= 1
BUILD     := packaging/build-image.sh
TAG_ARG   := $(if $(TAG),--tag $(TAG))
LATEST_ARG := $(if $(filter 0,$(LATEST)),--no-latest)
BUILD_DIR ?= build
FIXTURE   ?= $(BUILD_DIR)/av1-fixture.ts

.PHONY: image image-push image-pinned image-multiarch fixture fixture-decode help

image:
	IMAGE=$(IMAGE) $(BUILD) $(TAG_ARG) $(LATEST_ARG) --platform $(PLATFORMS)

# The publish path. Pinned base (docker/base-image.lock) so a rebuild of the
# same commit resolves the same Debian packages, single platform so it can be
# pushed as a plain image rather than a manifest list, tags <sha> + :latest.
# Needs a registry login out of band; see docs/IMAGE.md.
#
# LATEST=0 suppresses the :latest tag: use it for a probe or throwaway tag that
# must not repoint what sessions pull.
image-push:
	IMAGE=$(IMAGE) $(BUILD) $(TAG_ARG) $(LATEST_ARG) --platform $(PLATFORMS) --pinned --push

image-pinned:
	IMAGE=$(IMAGE) $(BUILD) $(TAG_ARG) $(LATEST_ARG) --platform $(PLATFORMS) --pinned

image-multiarch:
	IMAGE=$(IMAGE) $(BUILD) $(TAG_ARG) --platform linux/amd64,linux/arm64 --pinned --push

# One command, no arguments needed: the fixture is muxed the way the product's
# encoder muxes AV1 -- mpegtsmux alignment=7 enable-custom-mappings=true after
# av1parse -- and then checked through tsdemux/av1parse before the script exits.
fixture:
	./test/tools/make-av1-ts-fixture.sh $(FIXTURE)

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S . -B $(BUILD_DIR)

$(BUILD_DIR)/test/decode_file: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target decode_file -j

# decode_file drives the receiver's own decoder object over the fixture through
# a real ts_ring at RIST datagram granularity and prints
# `demux= dec= sink= frames=`. `frames=` is the measurement.
fixture-decode: fixture $(BUILD_DIR)/test/decode_file
	$(BUILD_DIR)/test/decode_file $(FIXTURE) av1

help:
	@sed -n '3,21p' Makefile
