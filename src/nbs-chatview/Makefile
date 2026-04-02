CC = gcc
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -Wno-format-truncation -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2

CHAT_DIR = ../nbs-chat
COMMON_DIR = ../nbs-common

TARGET = libchatview.a
TEST_UNIT = test_chatview_unit

# chatview.c + dependencies it needs from nbs-chat and nbs-common
OBJS = chatview.o chat_file.o base64.o lock.o render.o nbs_term_attr.o

# Source files needed by test (compiled directly, not via .o)
TEST_SRCS = chatview.c $(CHAT_DIR)/chat_file.c $(CHAT_DIR)/base64.c \
            $(CHAT_DIR)/lock.c $(CHAT_DIR)/render.c $(COMMON_DIR)/nbs_term_attr.c

.PHONY: all clean test debug asan

all: $(TARGET) $(TEST_UNIT)

$(TARGET): $(OBJS)
	ar rcs $@ $^

chatview.o: chatview.c chatview.h $(CHAT_DIR)/chat_file.h $(CHAT_DIR)/render.h $(CHAT_DIR)/handle_styles.h
	$(CC) $(CFLAGS) -c -o $@ chatview.c

chat_file.o: $(CHAT_DIR)/chat_file.c $(CHAT_DIR)/chat_file.h $(CHAT_DIR)/base64.h $(CHAT_DIR)/lock.h
	$(CC) $(CFLAGS) -c -o $@ $(CHAT_DIR)/chat_file.c

base64.o: $(CHAT_DIR)/base64.c $(CHAT_DIR)/base64.h
	$(CC) $(CFLAGS) -c -o $@ $(CHAT_DIR)/base64.c

lock.o: $(CHAT_DIR)/lock.c $(CHAT_DIR)/lock.h $(CHAT_DIR)/chat_file.h
	$(CC) $(CFLAGS) -c -o $@ $(CHAT_DIR)/lock.c

render.o: $(CHAT_DIR)/render.c $(CHAT_DIR)/render.h $(CHAT_DIR)/chat_file.h $(COMMON_DIR)/nbs_term_attr.h
	$(CC) $(CFLAGS) -c -o $@ $(CHAT_DIR)/render.c

nbs_term_attr.o: $(COMMON_DIR)/nbs_term_attr.c $(COMMON_DIR)/nbs_term_attr.h $(COMMON_DIR)/nbs_assert.h
	$(CC) $(CFLAGS) -c -o $@ $(COMMON_DIR)/nbs_term_attr.c

$(TEST_UNIT): test_chatview_unit.c chatview.c chatview.h $(CHAT_DIR)/chat_file.h
	$(CC) $(CFLAGS) -I. -I$(CHAT_DIR) -I$(COMMON_DIR) -o $@ test_chatview_unit.c $(TEST_SRCS)

test: $(TEST_UNIT)
	./$(TEST_UNIT)

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

asan: CC = clang
asan: CFLAGS = $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean all
	./$(TEST_UNIT)

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_UNIT) test_chatview_unit_asan
