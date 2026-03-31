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

# Ensure git submodules are initialised before building
submodules:
	@if [ ! -f lib/honest/Makefile ]; then \
		echo "Initialising git submodules..."; \
		git submodule update --init; \
	fi

all: submodules
	$(MAKE) -C src/nbs-bus
	$(MAKE) -C src/nbs-chat
	$(MAKE) -C src/nbs-chat-edit
	$(MAKE) -C src/nbs-sidecar
	$(MAKE) -C src/nbs-ts
	$(MAKE) -C src/nbs-ts-render
	$(MAKE) -C src/nbs-ts-tools
	$(MAKE) -C src/nbs-workers
	$(MAKE) -C src/nbs-scribe-log
	$(MAKE) -C src/nbs-hub

install: all
	$(MAKE) -C src/nbs-bus install
	$(MAKE) -C src/nbs-chat install
	$(MAKE) -C src/nbs-chat-edit install
	$(MAKE) -C src/nbs-sidecar install
	$(MAKE) -C src/nbs-ts install
	$(MAKE) -C src/nbs-ts-render install
	$(MAKE) -C src/nbs-ts-tools install
	$(MAKE) -C src/nbs-workers install
	$(MAKE) -C src/nbs-scribe-log install
	$(MAKE) -C src/nbs-hub install
	@mkdir -p $(HOME)/.nbs/commands
	@cp claude_tools/*.md $(HOME)/.nbs/commands/
	@echo "Installed skill files to ~/.nbs/commands/"
	@mkdir -p $(HOME)/.nbs/bin
	@if [ -d lib/honest/build ]; then \
		cp lib/honest/build/honest-build lib/honest/build/honest-extract \
		   lib/honest/build/honest-fmt lib/honest/build/honest-get \
		   lib/honest/build/honest-parse $(HOME)/.nbs/bin/; \
		echo "Installed honest tools to ~/.nbs/bin/"; \
	fi

clean:
	$(MAKE) -C src/nbs-bus clean
	$(MAKE) -C src/nbs-chat clean
	-$(MAKE) -C src/nbs-chat-edit clean
	$(MAKE) -C src/nbs-sidecar clean
	-$(MAKE) -C src/nbs-ts clean
	-$(MAKE) -C src/nbs-ts-render clean
	-$(MAKE) -C src/nbs-ts-tools clean
	-$(MAKE) -C src/nbs-workers clean
	-$(MAKE) -C src/nbs-scribe-log clean
	-$(MAKE) -C src/nbs-hub clean

debug:
	$(MAKE) -C src/nbs-bus debug
	$(MAKE) -C src/nbs-chat debug
	$(MAKE) -C src/nbs-sidecar debug

test-unit: install
	$(MAKE) -C src/nbs-bus test-unit
	$(MAKE) -C src/nbs-chat test-unit
	$(MAKE) -C src/nbs-sidecar test-unit
	$(MAKE) -C src/nbs-workers test-unit

test: install
	$(MAKE) -C src/nbs-bus test
	$(MAKE) -C src/nbs-chat test
	$(MAKE) -C src/nbs-sidecar test
	$(MAKE) -C src/nbs-ts test
	$(MAKE) -C src/nbs-ts-render test
	$(MAKE) -C src/nbs-workers test
	$(MAKE) -C src/nbs-scribe-log test
	bash tests/automated/test_interrupt_pattern.sh
	bash tests/automated/test_auto_archive.sh

test-all: test-unit test
