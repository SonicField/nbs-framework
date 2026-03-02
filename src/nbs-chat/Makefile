CC = gcc

# Build modes
# Default: optimised with asserts ON (no -DNDEBUG).
# Asserts are executable specifications, not debugging aids.
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
LDFLAGS =

BIN_DIR = ../../bin
TEST_DIR = ../../tests/automated
UNIT_TEST_DIR = ../../tests

# Shared library objects (used by both binaries)
LIB_SRCS = chat_file.c base64.c lock.c bus_bridge.c time_parse.c
LIB_OBJS = $(LIB_SRCS:.c=.o)

# nbs-chat CLI
CLI_SRCS = main.c
CLI_OBJS = $(CLI_SRCS:.c=.o)
CLI_TARGET = nbs-chat

# nbs-chat-terminal
TERM_SRCS = terminal.c watchdog.c
TERM_OBJS = $(TERM_SRCS:.c=.o)
TERM_TARGET = nbs-chat-terminal

# nbs-chat-remote (SSH proxy, no lib dependencies)
REMOTE_SRCS = remote.c
REMOTE_OBJS = $(REMOTE_SRCS:.c=.o)
REMOTE_TARGET = nbs-chat-remote

# nbs-chat-web (HTTP server for browser-based chat viewer)
WEB_SRCS = web.c
WEB_OBJS = $(WEB_SRCS:.c=.o)
WEB_TARGET = nbs-chat-web

# NDEBUG verification
ASSERT_CHECK = assert_check

# Unit test binaries (built in tests/ directory)
UNIT_TEST_CHAT_FILE = $(UNIT_TEST_DIR)/test_chat_file_unit
UNIT_TEST_BASE64 = $(UNIT_TEST_DIR)/test_base64_unit
UNIT_TEST_BUS_BRIDGE = $(UNIT_TEST_DIR)/test_bus_bridge_unit
UNIT_TEST_LOCK = $(UNIT_TEST_DIR)/test_lock_unit
UNIT_TEST_REMOTE = $(UNIT_TEST_DIR)/test_remote_unit
UNIT_TEST_TERMINAL = $(UNIT_TEST_DIR)/test_terminal_unit
UNIT_TEST_TIME = $(UNIT_TEST_DIR)/test_chat_time_unit
UNIT_TEST_TRUNCATE = $(UNIT_TEST_DIR)/test_chat_truncate_unit
UNIT_TEST_CHAT_MAIN = $(UNIT_TEST_DIR)/test_chat_main_unit
UNIT_TEST_WEB = $(UNIT_TEST_DIR)/test_web_unit
UNIT_TEST_AUDIT = $(UNIT_TEST_DIR)/test_chat_file_audit
UNIT_TEST_WATCHDOG = $(UNIT_TEST_DIR)/test_watchdog_unit

.PHONY: all clean install debug asan test test-debug test-asan test-unit test-all

all: $(CLI_TARGET) $(TERM_TARGET) $(REMOTE_TARGET) $(WEB_TARGET) $(ASSERT_CHECK)

$(CLI_TARGET): $(CLI_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TERM_TARGET): $(TERM_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread

$(REMOTE_TARGET): $(REMOTE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Asset embedding: generate web_assets.h from HTML source
web_assets.h: web_assets/index.html embed_assets.sh
	bash embed_assets.sh > web_assets.h

$(WEB_TARGET): $(WEB_OBJS) $(LIB_OBJS)
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
	-cp $(WEB_TARGET) $(BIN_DIR)/

# --- Build modes ---

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

asan: CC = clang
asan: CFLAGS = $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS = -fsanitize=address,undefined
asan: clean all

# --- Unit test compilation ---

$(UNIT_TEST_CHAT_FILE): $(UNIT_TEST_DIR)/test_chat_file_unit.c chat_file.c lock.c base64.c chat_file.h lock.h base64.h
	$(CC) $(CFLAGS) -I. -o $@ $(UNIT_TEST_DIR)/test_chat_file_unit.c chat_file.c lock.c base64.c $(LDFLAGS)

$(UNIT_TEST_BASE64): $(UNIT_TEST_DIR)/test_base64_unit.c base64.c chat_file.c lock.c base64.h chat_file.h lock.h
	$(CC) $(CFLAGS) -I. -o $@ $(UNIT_TEST_DIR)/test_base64_unit.c base64.c chat_file.c lock.c $(LDFLAGS) -lpthread

$(UNIT_TEST_BUS_BRIDGE): $(UNIT_TEST_DIR)/test_bus_bridge_unit.c bus_bridge.c chat_file.c lock.c base64.c bus_bridge.h chat_file.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I. -o $@ $(UNIT_TEST_DIR)/test_bus_bridge_unit.c bus_bridge.c chat_file.c lock.c base64.c $(LDFLAGS)

$(UNIT_TEST_LOCK): $(UNIT_TEST_DIR)/test_lock_unit.c lock.c chat_file.c base64.c lock.h chat_file.h
	$(CC) $(CFLAGS) -I. -o $@ $(UNIT_TEST_DIR)/test_lock_unit.c lock.c chat_file.c base64.c $(LDFLAGS)

$(UNIT_TEST_REMOTE): $(UNIT_TEST_DIR)/test_remote_unit.c remote.c
	$(CC) $(CFLAGS) -o $@ $(UNIT_TEST_DIR)/test_remote_unit.c $(LDFLAGS)

$(UNIT_TEST_TERMINAL): $(UNIT_TEST_DIR)/test_terminal_unit.c
	$(CC) $(CFLAGS) -o $@ $(UNIT_TEST_DIR)/test_terminal_unit.c $(LDFLAGS)

$(UNIT_TEST_TIME): $(UNIT_TEST_DIR)/test_chat_time_unit.c time_parse.c time_parse.h
	$(CC) $(CFLAGS) -I. -o $@ $(UNIT_TEST_DIR)/test_chat_time_unit.c time_parse.c $(LDFLAGS)

$(UNIT_TEST_TRUNCATE): $(UNIT_TEST_DIR)/test_chat_truncate_unit.c chat_file.c lock.c base64.c chat_file.h lock.h base64.h
	$(CC) $(CFLAGS) -I. -o $@ $(UNIT_TEST_DIR)/test_chat_truncate_unit.c chat_file.c lock.c base64.c $(LDFLAGS)

$(UNIT_TEST_CHAT_MAIN): $(UNIT_TEST_DIR)/test_chat_main_unit.c chat_file.c lock.c base64.c time_parse.c chat_file.h time_parse.h
	$(CC) $(CFLAGS) -I. -o $@ $(UNIT_TEST_DIR)/test_chat_main_unit.c chat_file.c lock.c base64.c time_parse.c $(LDFLAGS)

$(UNIT_TEST_WEB): $(UNIT_TEST_DIR)/test_web_unit.c web.c chat_file.c lock.c base64.c bus_bridge.c web_assets.h chat_file.h bus_bridge.h
	$(CC) $(CFLAGS) -Wno-format-truncation -I. -o $@ $(UNIT_TEST_DIR)/test_web_unit.c chat_file.c lock.c base64.c bus_bridge.c $(LDFLAGS)

$(UNIT_TEST_AUDIT): $(UNIT_TEST_DIR)/test_chat_file_audit.c chat_file.c lock.c base64.c chat_file.h lock.h base64.h
	$(CC) $(CFLAGS) -I. -o $@ $(UNIT_TEST_DIR)/test_chat_file_audit.c chat_file.c lock.c base64.c $(LDFLAGS)

$(UNIT_TEST_WATCHDOG): $(UNIT_TEST_DIR)/test_watchdog_unit.c watchdog.c watchdog.h ../nbs-common/nbs_assert.h
	$(CC) $(CFLAGS) -I. -o $@ $(UNIT_TEST_DIR)/test_watchdog_unit.c watchdog.c $(LDFLAGS)

# --- Test targets ---

test: all install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_chat_lifecycle.sh
	bash $(TEST_DIR)/test_nbs_chat_terminal.sh
	bash $(TEST_DIR)/test_nbs_chat_bus.sh
	bash $(TEST_DIR)/test_nbs_chat_web.sh

test-debug: debug install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_chat_lifecycle.sh
	bash $(TEST_DIR)/test_nbs_chat_terminal.sh
	bash $(TEST_DIR)/test_nbs_chat_bus.sh
	bash $(TEST_DIR)/test_nbs_chat_web.sh

test-asan: asan install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_chat_lifecycle.sh
	bash $(TEST_DIR)/test_nbs_chat_terminal.sh
	bash $(TEST_DIR)/test_nbs_chat_bus.sh
	bash $(TEST_DIR)/test_nbs_chat_web.sh

test-unit: $(UNIT_TEST_CHAT_FILE) $(UNIT_TEST_BASE64) $(UNIT_TEST_BUS_BRIDGE) $(UNIT_TEST_LOCK) $(UNIT_TEST_REMOTE) $(UNIT_TEST_TERMINAL) $(UNIT_TEST_TIME) $(UNIT_TEST_TRUNCATE) $(UNIT_TEST_WEB) $(UNIT_TEST_CHAT_MAIN) $(UNIT_TEST_AUDIT) $(UNIT_TEST_WATCHDOG)
	$(UNIT_TEST_CHAT_FILE)
	$(UNIT_TEST_BASE64)
	$(UNIT_TEST_BUS_BRIDGE)
	$(UNIT_TEST_LOCK)
	$(UNIT_TEST_REMOTE)
	$(UNIT_TEST_TERMINAL)
	$(UNIT_TEST_TIME)
	$(UNIT_TEST_TRUNCATE)
	$(UNIT_TEST_WEB)
	cd $(UNIT_TEST_DIR) && ./test_chat_main_unit
	$(UNIT_TEST_AUDIT)
	$(UNIT_TEST_WATCHDOG)

# Full test run: release + ASan
test-all: test-unit test test-asan

# Dependencies
main.o: main.c chat_file.h bus_bridge.h time_parse.h
terminal.o: terminal.c chat_file.h bus_bridge.h watchdog.h
watchdog.o: watchdog.c watchdog.h ../nbs-common/nbs_assert.h
chat_file.o: chat_file.c chat_file.h base64.h lock.h
base64.o: base64.c base64.h ../nbs-common/nbs_assert.h
lock.o: lock.c lock.h chat_file.h
bus_bridge.o: bus_bridge.c bus_bridge.h chat_file.h
remote.o: remote.c
web.o: web.c web_assets.h chat_file.h

clean:
	rm -f $(LIB_OBJS) $(CLI_OBJS) $(TERM_OBJS) $(REMOTE_OBJS) $(WEB_OBJS) $(CLI_TARGET) $(TERM_TARGET) $(REMOTE_TARGET) $(WEB_TARGET) $(ASSERT_CHECK) web_assets.h
	rm -f $(UNIT_TEST_CHAT_FILE) $(UNIT_TEST_BASE64) $(UNIT_TEST_BUS_BRIDGE) $(UNIT_TEST_LOCK) $(UNIT_TEST_REMOTE) $(UNIT_TEST_TERMINAL) $(UNIT_TEST_TIME) $(UNIT_TEST_TRUNCATE) $(UNIT_TEST_WEB) $(UNIT_TEST_CHAT_MAIN) $(UNIT_TEST_AUDIT) $(UNIT_TEST_WATCHDOG)
