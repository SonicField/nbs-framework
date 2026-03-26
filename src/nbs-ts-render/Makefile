CC = gcc
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
BIN_DIR = ../../bin

TARGET = nbs-ts-render
TEST = test_nbs_ts_render

SRCS = main.c nbs_ts_render.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean install test debug asan

all: $(TARGET) $(TEST)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST): test_nbs_ts_render.c nbs_ts_render.c
	$(CC) $(CFLAGS) -I../nbs-common -o $@ test_nbs_ts_render.c nbs_ts_render.c

%.o: %.c
	$(CC) $(CFLAGS) -I../nbs-common -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	install $(TARGET) $(BIN_DIR)/

test: $(TEST)
	./$(TEST)

clean:
	rm -f $(OBJS) $(TARGET) $(TEST)

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

asan: CC = clang
asan: CFLAGS = $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean all
