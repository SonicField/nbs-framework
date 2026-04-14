CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 \
         -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2
INCLUDES = -I../nbs-common -I../nbs-ts-render
BIN_DIR = ../../bin

SRCS = main.c ../nbs-common/nbs_term_attr.c ../nbs-ts-render/nbs_ts_wcwidth.c
TARGET = nbs-file-browser

# Runtime dependencies
REQUIRED_BINS = bat nbs-md-viewer

all: check-deps $(TARGET)

check-deps:
	@for bin in $(REQUIRED_BINS); do \
		command -v $$bin >/dev/null 2>&1 || \
		{ echo "Error: required binary '$$bin' not found in PATH" >&2; exit 1; }; \
	done

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SRCS)

install: all
	mkdir -p $(BIN_DIR)
	install $(TARGET) $(BIN_DIR)/

clean:
	rm -f $(TARGET)

.PHONY: all clean check-deps install
