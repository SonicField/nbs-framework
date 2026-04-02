CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2
CFLAGS  += -I../../lib/honest/include

TARGET   = nbs-help
BIN_DIR  = ../../bin
HONEST_LIB = ../../lib/honest/build/libhonest.a

.PHONY: all clean install

all: $(TARGET)

$(TARGET): nbs-help.c $(HONEST_LIB)
	$(CC) $(CFLAGS) -o $@ nbs-help.c $(HONEST_LIB)

$(HONEST_LIB):
	$(MAKE) -C ../../lib/honest lib

install: all
	mkdir -p $(BIN_DIR)
	rm -f $(BIN_DIR)/$(TARGET)
	install $(TARGET) $(BIN_DIR)/

clean:
	rm -f $(TARGET)
