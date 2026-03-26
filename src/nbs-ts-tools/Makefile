CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Werror -Wno-format-truncation -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2
BIN_DIR = ../../bin

TARGETS = nbs-ts-grep nbs-ts-query

.PHONY: all clean install

all: $(TARGETS)

nbs-ts-grep: nbs_ts_grep.c strip_ansi.c strip_ansi.h
	$(CC) $(CFLAGS) -I../nbs-common -o $@ nbs_ts_grep.c strip_ansi.c

nbs-ts-query: nbs_ts_query.c strip_ansi.c strip_ansi.h
	$(CC) $(CFLAGS) -I../nbs-common -o $@ nbs_ts_query.c strip_ansi.c

install: all
	mkdir -p $(BIN_DIR)
	install nbs-ts-grep $(BIN_DIR)/
	install nbs-ts-query $(BIN_DIR)/

clean:
	rm -f $(TARGETS)
