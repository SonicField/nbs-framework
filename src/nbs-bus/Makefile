CC = gcc

# Build modes
# Default: optimised with asserts ON (no -DNDEBUG).
# Asserts are executable specifications, not debugging aids.
COMMON_CFLAGS = -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
LDFLAGS =

BIN_DIR = ../../bin
TEST_DIR = ../../tests/automated

# Bus sources
BUS_SRCS = main.c bus.c
BUS_OBJS = $(BUS_SRCS:.c=.o)
BUS_TARGET = nbs-bus

# NDEBUG verification
ASSERT_CHECK = assert_check

.PHONY: all clean install debug asan test test-debug test-asan test-all

all: $(BUS_TARGET) $(ASSERT_CHECK)

$(BUS_TARGET): $(BUS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(ASSERT_CHECK): assert_check.c
	$(CC) $(CFLAGS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	cp $(BUS_TARGET) $(BIN_DIR)/

# --- Build modes ---

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

asan: CC = clang
asan: CFLAGS = $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS = -fsanitize=address,undefined
asan: clean all

# --- Test targets ---

test: clean all install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_bus.sh

test-debug: debug install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_bus.sh

test-asan: asan install
	./$(ASSERT_CHECK)
	bash $(TEST_DIR)/test_nbs_bus.sh

# Full test run: release + ASan
test-all: test test-asan
	@echo "=== All test modes passed ==="

# Dependencies
main.o: main.c bus.h
bus.o: bus.c bus.h

clean:
	rm -f $(BUS_OBJS) $(BUS_TARGET) $(ASSERT_CHECK)
