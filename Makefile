# NBS Framework — Top-level build
#
# Single entry point for building all components.
#
# Usage:
#   make          — build all binaries
#   make install  — build and install to bin/
#   make test     — build, install, and run all tests
#   make clean    — clean all build artifacts

.PHONY: all install test clean debug asan test-unit test-all

all:
	$(MAKE) -C src/nbs-bus
	$(MAKE) -C src/nbs-chat
	$(MAKE) -C src/nbs-sidecar
	$(MAKE) -C src/nbs-pty-session
	$(MAKE) -C src/nbs-workers

install: all
	$(MAKE) -C src/nbs-bus install
	$(MAKE) -C src/nbs-chat install
	$(MAKE) -C src/nbs-sidecar install
	$(MAKE) -C src/nbs-pty-session install
	$(MAKE) -C src/nbs-workers install

clean:
	$(MAKE) -C src/nbs-bus clean
	$(MAKE) -C src/nbs-chat clean
	$(MAKE) -C src/nbs-sidecar clean
	-$(MAKE) -C src/nbs-pty-session clean
	-$(MAKE) -C src/nbs-workers clean

debug:
	$(MAKE) -C src/nbs-bus debug
	$(MAKE) -C src/nbs-chat debug
	$(MAKE) -C src/nbs-sidecar debug

test-unit: install
	$(MAKE) -C src/nbs-bus test-unit
	$(MAKE) -C src/nbs-chat test-unit
	$(MAKE) -C src/nbs-sidecar test-unit

test: install
	$(MAKE) -C src/nbs-bus test
	$(MAKE) -C src/nbs-chat test
	$(MAKE) -C src/nbs-sidecar test
	$(MAKE) -C src/nbs-pty-session test
	$(MAKE) -C src/nbs-workers test
	bash tests/automated/test_interrupt_pattern.sh
	bash tests/automated/test_auto_archive.sh

test-all: test-unit test
