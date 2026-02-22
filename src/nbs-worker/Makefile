CC = gcc

# Build modes
# Default: optimised with asserts ON (no -DNDEBUG).
# Asserts are executable specifications, not debugging aids.
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2

BIN_DIR = ../../bin

SRCS = main.c worker.c
OBJS = $(SRCS:.c=.o)
TARGET = nbs-worker

# NDEBUG verification
ASSERT_CHECK = assert_check

.PHONY: all clean install debug

all: $(TARGET) $(ASSERT_CHECK)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -I../nbs-common -c -o $@ $<

$(ASSERT_CHECK): assert_check.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	cp $(TARGET) $(BIN_DIR)/nbs-worker

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

clean:
	rm -f $(OBJS) $(TARGET) $(ASSERT_CHECK)

# Dependencies
main.o: main.c worker.h
worker.o: worker.c worker.h
