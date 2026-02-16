CC = gcc

# Build modes
# Default: optimised with asserts ON (no -DNDEBUG).
# Asserts are executable specifications, not debugging aids.
COMMON_CFLAGS = -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
LDFLAGS =

BIN_DIR = ../../bin
TEST_DIR = ../../tests/automated

# Shared library objects (used by both binaries)
LIB_SRCS = chat_file.c base64.c lock.c bus_bridge.c
LIB_OBJS = $(LIB_SRCS:.c=.o)

# nbs-chat CLI
CLI_SRCS = main.c
CLI_OBJS = $(CLI_SRCS:.c=.o)
CLI_TARGET = nbs-chat

# nbs-chat-terminal
TERM_SRCS = terminal.c
TERM_OBJS = $(TERM_SRCS:.c=.o)
TERM_TARGET = nbs-chat-terminal

# nbs-chat-remote (SSH proxy, no lib dependencies)
REMOTE_SRCS = remote.c
REMOTE_OBJS = $(REMOTE_SRCS:.c=.o)
REMOTE_TARGET = nbs-chat-remote

# NDEBUG verification
ASSERT_CHECK = assert_check

.PHONY: all clean install debug asan test test-debug test-asan test-all

all: $(CLI_TARGET) $(TERM_TARGET) $(REMOTE_TARGET) $(ASSERT_CHECK)

$(CLI_TARGET): $(CLI_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TERM_TARGET): $(TERM_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(REMOTE_TARGET): $(REMOTE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(ASSERT_CHECK): assert_check.c
	$(CC) $(CFLAGS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	cp $(CLI_TARGET) $(BIN_DIR)/
	cp $(TERM_TARGET) $(BIN_DIR)/
	cp $(REMOTE_TARGET) $(BIN_DIR)/

# --- Build modes ---

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

asan: CC = clang
asan: CFLAGS = $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS = -fsanitize=address,undefined
asan: clean all

# --- Test targets ---

test: all install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_chat_lifecycle.sh
	bash $(TEST_DIR)/test_nbs_chat_terminal.sh
	bash $(TEST_DIR)/test_nbs_chat_bus.sh

test-debug: debug install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_chat_lifecycle.sh
	bash $(TEST_DIR)/test_nbs_chat_terminal.sh
	bash $(TEST_DIR)/test_nbs_chat_bus.sh

test-asan: asan install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_chat_lifecycle.sh
	bash $(TEST_DIR)/test_nbs_chat_terminal.sh
	bash $(TEST_DIR)/test_nbs_chat_bus.sh

# Full test run: release + ASan
test-all: test test-asan

# Dependencies
main.o: main.c chat_file.h bus_bridge.h
terminal.o: terminal.c chat_file.h bus_bridge.h
chat_file.o: chat_file.c chat_file.h base64.h lock.h
base64.o: base64.c base64.h chat_file.h
lock.o: lock.c lock.h chat_file.h
bus_bridge.o: bus_bridge.c bus_bridge.h chat_file.h
remote.o: remote.c

clean:
	rm -f $(LIB_OBJS) $(CLI_OBJS) $(TERM_OBJS) $(REMOTE_OBJS) $(CLI_TARGET) $(TERM_TARGET) $(REMOTE_TARGET) $(ASSERT_CHECK)
