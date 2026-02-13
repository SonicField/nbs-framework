CC = gcc

# Build modes
# Default: optimised with asserts ON (no -DNDEBUG).
# Asserts are executable specifications, not debugging aids.
COMMON_CFLAGS = -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
LDFLAGS =

BIN_DIR = ../../bin
TEST_DIR = ../../tests/automated

# Shared library sources from nbs-chat
CHAT_DIR = ../nbs-chat
SHARED_SRCS = $(CHAT_DIR)/chat_file.c $(CHAT_DIR)/base64.c $(CHAT_DIR)/lock.c
SHARED_OBJS = chat_file.o base64.o lock.o

# Hub sources
HUB_SRCS = nbs-hub.c hub_state.c hub_commands.c hub_log.c
HUB_OBJS = $(HUB_SRCS:.c=.o)
HUB_TARGET = nbs-hub

# NDEBUG verification
ASSERT_CHECK = assert_check

.PHONY: all clean install debug asan test test-debug test-asan test-all

all: $(HUB_TARGET) $(ASSERT_CHECK)

$(HUB_TARGET): $(HUB_OBJS) $(SHARED_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(ASSERT_CHECK): $(CHAT_DIR)/assert_check.c
	$(CC) $(CFLAGS) -o $@ $<

# Hub source compilation
%.o: %.c
	$(CC) $(CFLAGS) -I$(CHAT_DIR) -c -o $@ $<

# Shared library compilation — build .o files from nbs-chat sources
chat_file.o: $(CHAT_DIR)/chat_file.c
	$(CC) $(CFLAGS) -I$(CHAT_DIR) -c -o $@ $<

base64.o: $(CHAT_DIR)/base64.c
	$(CC) $(CFLAGS) -I$(CHAT_DIR) -c -o $@ $<

lock.o: $(CHAT_DIR)/lock.c
	$(CC) $(CFLAGS) -I$(CHAT_DIR) -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	cp $(HUB_TARGET) $(BIN_DIR)/

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
	bash $(TEST_DIR)/test_nbs_hub.sh

test-debug: debug install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_hub.sh

test-asan: asan install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_hub.sh

# Full test run: release + ASan
test-all: test test-asan

# Dependencies
nbs-hub.o: nbs-hub.c hub_state.h hub_commands.h hub_log.h
hub_state.o: hub_state.c hub_state.h
hub_commands.o: hub_commands.c hub_commands.h hub_state.h hub_log.h
hub_log.o: hub_log.c hub_log.h hub_state.h

clean:
	rm -f $(HUB_OBJS) $(SHARED_OBJS) $(HUB_TARGET) $(ASSERT_CHECK)
