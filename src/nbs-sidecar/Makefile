CC = gcc

# Build modes
# Default: optimised with asserts ON (no -DNDEBUG).
# Asserts are executable specifications, not debugging aids.
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
LDFLAGS =
INCLUDES = -I. -I../nbs-common -I../nbs-chat

BIN_DIR = ../../bin
TEST_DIR = ../../tests/automated
UNIT_TEST_DIR = ../../tests

# nbs-chat sources needed for base64, lock
# Compiled locally to avoid cross-directory .o contamination (ASan/non-ASan mismatch)
CHAT_SRC_DIR = ../nbs-chat
CHAT_LIB_SRCS = $(CHAT_SRC_DIR)/base64.c $(CHAT_SRC_DIR)/lock.c $(CHAT_SRC_DIR)/bus_bridge.c
CHAT_LIB_OBJS = chat_base64.o chat_lock.o chat_bus_bridge.o

# Sidecar sources
SRCS = main.c sidecar.c transport_ts.c detect.c exec_util.c \
       bus_client.c chat_client.c registry.c triggers.c hash.c strip_ansi.c \
       mention_escape.c
OBJS = $(SRCS:.c=.o)
TARGET = nbs-sidecar

ASSERT_CHECK = assert_check

# Unit test sources
UNIT_TESTS = $(UNIT_TEST_DIR)/test_sidecar_detect_unit \
             $(UNIT_TEST_DIR)/test_sidecar_hash_unit \
             $(UNIT_TEST_DIR)/test_sidecar_exec_unit \
             $(UNIT_TEST_DIR)/test_sidecar_registry_unit \
             $(UNIT_TEST_DIR)/test_sidecar_escape_mentions_unit \
             $(UNIT_TEST_DIR)/test_sidecar_strip_ansi_unit \
             $(UNIT_TEST_DIR)/test_sidecar_triggers_unit \
             $(UNIT_TEST_DIR)/test_sidecar_transport_unit \
             $(UNIT_TEST_DIR)/test_sidecar_main_sidecar_unit \
             $(UNIT_TEST_DIR)/test_sidecar_bus_client_unit \
             $(UNIT_TEST_DIR)/test_sidecar_chat_client_unit \
             $(UNIT_TEST_DIR)/test_bus_client_deferred_ack_unit

.PHONY: all clean install debug asan test test-unit test-all

all: $(TARGET) $(ASSERT_CHECK)

# Build nbs-chat sources locally (avoids cross-directory .o contamination)
chat_base64.o: $(CHAT_SRC_DIR)/base64.c $(CHAT_SRC_DIR)/base64.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(CHAT_SRC_DIR)/base64.c

chat_lock.o: $(CHAT_SRC_DIR)/lock.c $(CHAT_SRC_DIR)/lock.h $(CHAT_SRC_DIR)/chat_file.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(CHAT_SRC_DIR)/lock.c

chat_bus_bridge.o: $(CHAT_SRC_DIR)/bus_bridge.c $(CHAT_SRC_DIR)/bus_bridge.h $(CHAT_SRC_DIR)/chat_file.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(CHAT_SRC_DIR)/bus_bridge.c

$(TARGET): $(OBJS) $(CHAT_LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(ASSERT_CHECK): assert_check.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	rm -f $(BIN_DIR)/$(TARGET)
	install $(TARGET) $(BIN_DIR)/

clean:
	rm -f $(OBJS) $(CHAT_LIB_OBJS) $(TARGET) $(ASSERT_CHECK)
	rm -f $(UNIT_TESTS)

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

asan: CC = clang
asan: CFLAGS = $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS = -fsanitize=address,undefined
asan: clean all

# Unit test build rules
$(UNIT_TEST_DIR)/test_sidecar_detect_unit: $(UNIT_TEST_DIR)/test_sidecar_detect_unit.c detect.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_sidecar_hash_unit: $(UNIT_TEST_DIR)/test_sidecar_hash_unit.c hash.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_sidecar_exec_unit: $(UNIT_TEST_DIR)/test_sidecar_exec_unit.c exec_util.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_sidecar_registry_unit: $(UNIT_TEST_DIR)/test_sidecar_registry_unit.c registry.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_sidecar_escape_mentions_unit: $(UNIT_TEST_DIR)/test_sidecar_escape_mentions_unit.c mention_escape.o chat_bus_bridge.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_sidecar_strip_ansi_unit: $(UNIT_TEST_DIR)/test_sidecar_strip_ansi_unit.c strip_ansi.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_sidecar_triggers_unit: $(UNIT_TEST_DIR)/test_sidecar_triggers_unit.c triggers.o bus_client.o chat_client.o registry.o exec_util.o $(CHAT_LIB_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_sidecar_transport_unit: $(UNIT_TEST_DIR)/test_sidecar_transport_unit.c transport_ts.o exec_util.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_sidecar_main_sidecar_unit: $(UNIT_TEST_DIR)/test_sidecar_main_sidecar_unit.c sidecar.o detect.o hash.o bus_client.o chat_client.o registry.o triggers.o exec_util.o strip_ansi.o mention_escape.o transport_ts.o $(CHAT_LIB_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_sidecar_bus_client_unit: $(UNIT_TEST_DIR)/test_sidecar_bus_client_unit.c bus_client.o exec_util.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_sidecar_chat_client_unit: $(UNIT_TEST_DIR)/test_sidecar_chat_client_unit.c chat_client.o registry.o exec_util.o $(CHAT_LIB_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(UNIT_TEST_DIR)/test_bus_client_deferred_ack_unit: $(UNIT_TEST_DIR)/test_bus_client_deferred_ack_unit.c bus_client.o exec_util.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

test-unit: $(UNIT_TESTS)
	$(UNIT_TEST_DIR)/test_sidecar_detect_unit
	$(UNIT_TEST_DIR)/test_sidecar_hash_unit
	$(UNIT_TEST_DIR)/test_sidecar_exec_unit
	$(UNIT_TEST_DIR)/test_sidecar_registry_unit
	$(UNIT_TEST_DIR)/test_sidecar_escape_mentions_unit
	$(UNIT_TEST_DIR)/test_sidecar_strip_ansi_unit
	$(UNIT_TEST_DIR)/test_sidecar_triggers_unit
	$(UNIT_TEST_DIR)/test_sidecar_transport_unit
	$(UNIT_TEST_DIR)/test_sidecar_main_sidecar_unit
	PATH="$(shell pwd)/../../bin:$$PATH" $(UNIT_TEST_DIR)/test_sidecar_bus_client_unit
	PATH="$(shell pwd)/../../bin:$$PATH" $(UNIT_TEST_DIR)/test_bus_client_deferred_ack_unit
	PATH="$(shell pwd)/../../bin:$$PATH" $(UNIT_TEST_DIR)/test_sidecar_chat_client_unit

test: all install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_sidecar_lifecycle.sh

test-asan: asan install
	bash $(TEST_DIR)/test_sidecar_lifecycle.sh

test-all: test-unit test
