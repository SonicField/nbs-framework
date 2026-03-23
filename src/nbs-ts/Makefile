CC = gcc
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
LDFLAGS = -lpthread -lutil
BIN_DIR = ../../bin

SRCS = main.c session.c io.c wait.c
OBJS = $(SRCS:.c=.o)
TARGET = nbs-ts

ASSERT_CHECK = assert_check

.PHONY: all clean install test debug asan

all: $(TARGET) $(ASSERT_CHECK)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(ASSERT_CHECK): assert_check.c
	$(CC) $(CFLAGS) -I../nbs-common -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -I../nbs-common -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	rm -f $(BIN_DIR)/$(TARGET)
	install $(TARGET) $(BIN_DIR)/

test: all install
	./$(ASSERT_CHECK)

clean:
	rm -f $(OBJS) $(TARGET) $(ASSERT_CHECK)

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

asan: CC = clang
asan: CFLAGS = $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS = -fsanitize=address,undefined -lpthread -lutil
asan: clean all
