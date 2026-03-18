CC = gcc
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
BIN_DIR = ../../bin

SRCS = main.c session.c
OBJS = $(SRCS:.c=.o)
TARGET = nbs-pty-session

ASSERT_CHECK = assert_check
TEST_TARGET = test_pty_session

.PHONY: all clean install test

all: $(TARGET) $(ASSERT_CHECK)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(ASSERT_CHECK): assert_check.c
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_TARGET): test_pty_session.c main.c session.c session.h
	$(CC) $(COMMON_CFLAGS) -O2 -DTEST_BUILD -I../nbs-common -o $@ test_pty_session.c main.c session.c

%.o: %.c
	$(CC) $(CFLAGS) -I../nbs-common -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	rm -f $(BIN_DIR)/pty-session
	install $(TARGET) $(BIN_DIR)/pty-session

test: all install $(TEST_TARGET)
	./$(ASSERT_CHECK)
	./$(TEST_TARGET)

clean:
	rm -f $(OBJS) $(TARGET) $(ASSERT_CHECK) $(TEST_TARGET)
