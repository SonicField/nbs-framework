CC = gcc
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -Wno-format-truncation \
                -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
                -D_XOPEN_SOURCE=700
CFLAGS ?= $(COMMON_CFLAGS) -O2

COMMON_DIR = ../nbs-common
TS_RENDER_DIR = ../nbs-ts-render
INCLUDES = -I$(COMMON_DIR) -I$(TS_RENDER_DIR)

TARGET = nbs-dashboard
LIB_TARGET = libdashboard.a

LIB_OBJS = dashboard.o nbs_term_attr.o nbs_ts_wcwidth.o
MAIN_OBJ = main.o

BIN_DIR = ../../bin

.PHONY: all clean install debug asan

all: $(TARGET) $(LIB_TARGET)

$(LIB_TARGET): $(LIB_OBJS)
	ar rcs $@ $^

$(TARGET): $(MAIN_OBJ) $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(MAIN_OBJ) $(LIB_TARGET)

dashboard.o: dashboard.c dashboard.h $(COMMON_DIR)/nbs_term_attr.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ dashboard.c

main.o: main.c dashboard.h
	$(CC) $(CFLAGS) -c -o $@ main.c

nbs_term_attr.o: $(COMMON_DIR)/nbs_term_attr.c $(COMMON_DIR)/nbs_term_attr.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(COMMON_DIR)/nbs_term_attr.c

nbs_ts_wcwidth.o: $(TS_RENDER_DIR)/nbs_ts_wcwidth.c $(TS_RENDER_DIR)/nbs_ts_wcwidth.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(TS_RENDER_DIR)/nbs_ts_wcwidth.c

install: all
	mkdir -p $(BIN_DIR)
	install $(TARGET) $(BIN_DIR)/

clean:
	rm -f $(LIB_OBJS) $(MAIN_OBJ) $(TARGET) $(LIB_TARGET)

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

asan: CC = clang
asan: CFLAGS = $(COMMON_CFLAGS) -O1 -g \
               -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean all
