CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Werror -Wno-format-truncation -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2
BIN_DIR = ../../bin

# Dependencies from nbs-chat (compiled as local .o files)
CHAT_DIR = ../nbs-chat
COMMON_DIR = ../nbs-common

TARGET = nbs-chat-edit

.PHONY: all clean install

all: $(TARGET)

$(TARGET): editor.c $(CHAT_DIR)/chat_file.c $(CHAT_DIR)/base64.c $(CHAT_DIR)/lock.c $(CHAT_DIR)/render.c $(COMMON_DIR)/nbs_term_attr.c
	$(CC) $(CFLAGS) -I$(CHAT_DIR) -o $@ editor.c \
		$(CHAT_DIR)/chat_file.c $(CHAT_DIR)/base64.c $(CHAT_DIR)/lock.c \
		$(CHAT_DIR)/render.c $(COMMON_DIR)/nbs_term_attr.c

install: all
	mkdir -p $(BIN_DIR)
	install $(TARGET) $(BIN_DIR)/

clean:
	rm -f $(TARGET)
