CC = gcc
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
BIN_DIR = ../../bin

SRCS = main.c session.c
OBJS = $(SRCS:.c=.o)
TARGET = nbs-pty-session

ASSERT_CHECK = assert_check

.PHONY: all clean install test

all: $(TARGET) $(ASSERT_CHECK)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(ASSERT_CHECK): assert_check.c
	$(CC) $(CFLAGS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -I../nbs-common -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	cp $(TARGET) $(BIN_DIR)/pty-session

test: all install
	./$(ASSERT_CHECK)

clean:
	rm -f $(OBJS) $(TARGET) $(ASSERT_CHECK)
