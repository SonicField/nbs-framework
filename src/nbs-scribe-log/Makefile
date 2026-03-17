CC = gcc

# Build modes
# Default: optimised with asserts ON (no -DNDEBUG).
# Asserts are executable specifications, not debugging aids.
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
LDFLAGS =

BIN_DIR = ../../bin
TEST_DIR = ../../tests/automated

# Sources
SRCS = main.c scribe_log.c
OBJS = $(SRCS:.c=.o)
TARGET = nbs-scribe-log

# NDEBUG verification
ASSERT_CHECK = assert_check

.PHONY: all clean install test

all: $(TARGET) $(ASSERT_CHECK)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(ASSERT_CHECK): assert_check.c
	$(CC) $(CFLAGS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -I../nbs-common -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	rm -f $(BIN_DIR)/$(TARGET)
	cp $(TARGET) $(BIN_DIR)/

test: clean all install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_scribe_log.sh

# Dependencies
main.o: main.c scribe_log.h
scribe_log.o: scribe_log.c scribe_log.h

clean:
	rm -f $(OBJS) $(TARGET) $(ASSERT_CHECK)
