CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Werror -Wno-format-truncation -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2
BIN_DIR = ../../bin

# libchatview provides TUI, navigation, search, rendering
CHATVIEW_DIR = ../nbs-chatview
CHATVIEW_LIB = $(CHATVIEW_DIR)/libchatview.a

TARGET = nbs-chat-edit

.PHONY: all clean install chatview

all: $(TARGET)

chatview:
	$(MAKE) -C $(CHATVIEW_DIR)

$(CHATVIEW_LIB): chatview

$(TARGET): editor.c $(CHATVIEW_LIB)
	$(CC) $(CFLAGS) -o $@ editor.c $(CHATVIEW_LIB)

install: all
	mkdir -p $(BIN_DIR)
	install $(TARGET) $(BIN_DIR)/

clean:
	rm -f $(TARGET)
