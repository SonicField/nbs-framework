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
	$(MAKE) -C src/nbs-chatview
	$(MAKE) -C src/nbs-chat
	$(MAKE) -C src/nbs-chat-edit
	$(MAKE) -C src/nbs-sidecar
	$(MAKE) -C src/nbs-ts
	$(MAKE) -C src/nbs-ts-render
	$(MAKE) -C src/nbs-ts-tools
	$(MAKE) -C src/nbs-workers
	$(MAKE) -C src/nbs-scribe-log
	$(MAKE) -C src/nbs-hub
	$(MAKE) -C src/nbs-md-viewer
	$(MAKE) -C src/nbs-help

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
	$(MAKE) -C src/nbs-md-viewer install
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
	$(MAKE) -C src/nbs-help install
	@cp MANIFEST.honest $(HOME)/.nbs/
	@rm -rf $(HOME)/.nbs/docs $(HOME)/.nbs/concepts $(HOME)/.nbs/terminal-weathering
	@cp -r docs $(HOME)/.nbs/docs
	@cp -r concepts $(HOME)/.nbs/concepts
	@mkdir -p $(HOME)/.nbs/lib/honest/docs $(HOME)/.nbs/lib/honest/specs
	@cp lib/honest/docs/*.md $(HOME)/.nbs/lib/honest/docs/ 2>/dev/null || true
	@cp lib/honest/specs/*.md $(HOME)/.nbs/lib/honest/specs/ 2>/dev/null || true
	@cp -r terminal-weathering $(HOME)/.nbs/terminal-weathering 2>/dev/null || true
	@echo "Installed manifest, docs, and concepts to ~/.nbs/"
	@if [ -f MANIFEST.honest ] && command -v honest-parse >/dev/null 2>&1; then \
		honest-parse MANIFEST.honest >/dev/null 2>&1 || \
			echo "WARNING: MANIFEST.honest has parse errors"; \
		for tool in bin/nbs-*; do \
			name=$$(basename "$$tool"); \
			if ! grep -q "'$$name'" MANIFEST.honest 2>/dev/null; then \
				echo "WARNING: $$name missing from MANIFEST.honest"; \
			fi; \
		done; \
	fi

clean:
	$(MAKE) -C src/nbs-bus clean
	-$(MAKE) -C src/nbs-chatview clean
	$(MAKE) -C src/nbs-chat clean
	-$(MAKE) -C src/nbs-chat-edit clean
	$(MAKE) -C src/nbs-sidecar clean
	-$(MAKE) -C src/nbs-ts clean
	-$(MAKE) -C src/nbs-ts-render clean
	-$(MAKE) -C src/nbs-ts-tools clean
	-$(MAKE) -C src/nbs-workers clean
	-$(MAKE) -C src/nbs-scribe-log clean
	-$(MAKE) -C src/nbs-hub clean
	-$(MAKE) -C src/nbs-md-viewer clean

debug:
	$(MAKE) -C src/nbs-bus debug
	$(MAKE) -C src/nbs-chat debug
	$(MAKE) -C src/nbs-sidecar debug

test-unit: install
	$(MAKE) -C src/nbs-bus test-unit
	$(MAKE) -C src/nbs-chatview test
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
	$(MAKE) -C src/nbs-md-viewer test
	bash tests/automated/test_interrupt_pattern.sh
	bash tests/automated/test_auto_archive.sh
	bash tests/automated/test_manifest_install.sh
	bash tests/automated/test_nbs_help.sh

test-all: test-unit test
