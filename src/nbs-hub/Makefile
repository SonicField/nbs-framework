CC = gcc

# Build modes
# Default: optimised with asserts ON (no -DNDEBUG).
# Asserts are executable specifications, not debugging aids.
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -Wno-format-truncation -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2

BIN_DIR = ../../bin

SRCS = main.c hub.c
OBJS = $(SRCS:.c=.o)
TARGET = nbs-hub

.PHONY: all clean install debug test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -I../nbs-common -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	rm -f $(BIN_DIR)/nbs-hub
	install $(TARGET) $(BIN_DIR)/nbs-hub

test: all install
	@echo "nbs-hub: build OK"

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

clean:
	rm -f $(OBJS) $(TARGET)

# Dependencies
main.o: main.c hub.h
hub.o: hub.c hub.h
