CC = gcc

# Build modes
# Default: optimised with asserts ON (no -DNDEBUG).
# Asserts are executable specifications, not debugging aids.
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2

BIN_DIR = ../../bin

# Honest library (session metadata)
HONEST_INC = -I../../lib/honest/include
HONEST_LIB = ../../lib/honest/build/libhonest.a

SRCS = main.c worker.c
OBJS = $(SRCS:.c=.o)
TARGET = nbs-workers

# NDEBUG verification
ASSERT_CHECK = assert_check

.PHONY: all clean install debug test test-unit honest-lib

all: honest-lib $(TARGET) $(ASSERT_CHECK)

# Build honest library if not already built
honest-lib:
	@if [ ! -f $(HONEST_LIB) ]; then \
		echo "Building honest library..."; \
		$(MAKE) -C ../../lib/honest; \
	fi

$(TARGET): $(OBJS) $(HONEST_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(HONEST_LIB)

%.o: %.c
	$(CC) $(CFLAGS) -I../nbs-common $(HONEST_INC) -c -o $@ $<

$(ASSERT_CHECK): assert_check.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	rm -f $(BIN_DIR)/nbs-workers
	install $(TARGET) $(BIN_DIR)/nbs-workers

test: all install
	./$(ASSERT_CHECK)

test-unit: all
	$(CC) $(CFLAGS) -I. -I../nbs-common $(HONEST_INC) -o ../../tests/test_workers_unit ../../tests/test_workers_unit.c worker.c $(HONEST_LIB)
	../../tests/test_workers_unit
	$(CC) $(CFLAGS) -I. -I../nbs-common $(HONEST_INC) -o ../../tests/test_workers_adversarial ../../tests/test_workers_adversarial.c worker.c $(HONEST_LIB)
	../../tests/test_workers_adversarial

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

clean:
	rm -f $(OBJS) $(TARGET) $(ASSERT_CHECK)

# Dependencies
main.o: main.c worker.h
worker.o: worker.c worker.h
