CC = gcc
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
LDFLAGS = -lpthread -lutil
BIN_DIR = ../../bin

COMMON_DIR = ../nbs-common
SRCS = main.c session.c io.c wait.c helper_client.c
OBJS = $(SRCS:.c=.o)
COMMON_OBJS = nbs_helper_check.o
TARGET = nbs-ts
HELPER = nbs-ts-helper

ASSERT_CHECK = assert_check

.PHONY: all clean install test debug asan test-valgrind

all: $(TARGET) $(HELPER) $(ASSERT_CHECK)

$(HELPER): helper.o
	$(CC) $(CFLAGS) -o $@ $^ -lutil

$(TARGET): $(OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(ASSERT_CHECK): assert_check.c
	$(CC) $(CFLAGS) -I../nbs-common -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -I../nbs-common -c -o $@ $<

nbs_helper_check.o: $(COMMON_DIR)/nbs_helper_check.c $(COMMON_DIR)/nbs_helper_check.h
	$(CC) $(CFLAGS) -I../nbs-common -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	rm -f $(BIN_DIR)/$(TARGET) $(BIN_DIR)/$(HELPER)
	install $(TARGET) $(BIN_DIR)/
	install $(HELPER) $(BIN_DIR)/

test: all install
	./$(ASSERT_CHECK)

clean:
	rm -f $(OBJS) $(COMMON_OBJS) helper.o $(TARGET) $(HELPER) $(ASSERT_CHECK)

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

asan: CC = clang
asan: CFLAGS = $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS = -fsanitize=address,undefined -lpthread -lutil
asan: clean all

test-valgrind: CFLAGS = $(COMMON_CFLAGS) -O0 -g
test-valgrind: clean all install
	bash ../../tests/valgrind/test_helper_valgrind.sh
