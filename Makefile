# Container image build and decode-fixture targets for the receiver.
#
# Image: packaging/build-image.sh tags the image with the git commit's short
# SHA plus :latest. See docs/IMAGE.md for the full contract.
#
#   make image            # local linux/amd64 image, tags <sha> and :latest
#   make image-pinned     # same, base image pinned to docker/base-image.lock
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
BUILD     := packaging/build-image.sh
TAG_ARG   := $(if $(TAG),--tag $(TAG))
BUILD_DIR ?= build
FIXTURE   ?= $(BUILD_DIR)/av1-fixture.ts

.PHONY: image image-pinned image-multiarch fixture fixture-decode help

image:
	IMAGE=$(IMAGE) $(BUILD) $(TAG_ARG) --platform $(PLATFORMS)

image-pinned:
	IMAGE=$(IMAGE) $(BUILD) $(TAG_ARG) --platform $(PLATFORMS) --pinned

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
