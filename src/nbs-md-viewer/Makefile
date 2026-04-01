CC = gcc
COMMON_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(COMMON_CFLAGS) -O2
BIN_DIR = ../../bin

INCLUDES = -I../nbs-common -I../nbs-ts-render

# Highlight sources needed by any target that links md_render.c
HL_SRCS = md_highlight.c md_lang_c.c md_lang_js.c md_lang_py.c md_lang_pas.c

TARGET = nbs-md-viewer
TEST_AST = test_md_ast
TEST_PARSE = test_md_parse
TEST_RENDER = test_md_render
TEST_TABLE = test_md_table
TEST_VIEWPORT = test_md_viewport
TEST_HIGHLIGHT = test_md_highlight

SRCS = main.c md_ast.c md_parse.c md_render.c md_table.c md_style.c md_viewport.c md_terminal.c \
       md_highlight.c md_lang_c.c md_lang_js.c md_lang_py.c md_lang_pas.c \
       ../nbs-common/nbs_term_attr.c ../nbs-ts-render/nbs_ts_wcwidth.c ../nbs-ts-render/nbs_ts_bidi.c
OBJS = $(SRCS:.c=.o)

TESTS = $(TEST_AST) $(TEST_PARSE) $(TEST_RENDER) $(TEST_TABLE) $(TEST_VIEWPORT) $(TEST_HIGHLIGHT)

.PHONY: all clean install test debug asan

all: $(TARGET) $(TESTS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_AST): test_md_ast.c md_ast.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_md_ast.c md_ast.c

$(TEST_PARSE): test_md_parse.c md_ast.c md_parse.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_md_parse.c md_ast.c md_parse.c

$(TEST_RENDER): test_md_render.c md_render.c md_parse.c md_ast.c md_style.c md_table.c $(HL_SRCS) ../nbs-common/nbs_term_attr.c ../nbs-ts-render/nbs_ts_wcwidth.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_md_render.c md_render.c md_parse.c md_ast.c md_style.c md_table.c $(HL_SRCS) ../nbs-common/nbs_term_attr.c ../nbs-ts-render/nbs_ts_wcwidth.c

$(TEST_TABLE): test_md_table.c md_table.c md_render.c md_parse.c md_ast.c md_style.c $(HL_SRCS) ../nbs-common/nbs_term_attr.c ../nbs-ts-render/nbs_ts_wcwidth.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_md_table.c md_table.c md_render.c md_parse.c md_ast.c md_style.c $(HL_SRCS) ../nbs-common/nbs_term_attr.c ../nbs-ts-render/nbs_ts_wcwidth.c

$(TEST_VIEWPORT): test_md_viewport.c md_viewport.c md_terminal.c md_render.c md_parse.c md_ast.c md_style.c md_table.c $(HL_SRCS) ../nbs-common/nbs_term_attr.c ../nbs-ts-render/nbs_ts_wcwidth.c ../nbs-ts-render/nbs_ts_bidi.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_md_viewport.c md_viewport.c md_terminal.c md_render.c md_parse.c md_ast.c md_style.c md_table.c $(HL_SRCS) ../nbs-common/nbs_term_attr.c ../nbs-ts-render/nbs_ts_wcwidth.c ../nbs-ts-render/nbs_ts_bidi.c

$(TEST_HIGHLIGHT): test_md_highlight.c md_highlight.c md_lang_c.c md_lang_js.c md_lang_py.c md_lang_pas.c md_style.c ../nbs-common/nbs_term_attr.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_md_highlight.c md_highlight.c md_lang_c.c md_lang_js.c md_lang_py.c md_lang_pas.c md_style.c ../nbs-common/nbs_term_attr.c

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	install $(TARGET) $(BIN_DIR)/

test: $(TESTS)
	./$(TEST_AST)
	./$(TEST_PARSE)
	./$(TEST_RENDER)
	./$(TEST_TABLE)
	./$(TEST_VIEWPORT)
	./$(TEST_HIGHLIGHT)

clean:
	rm -f $(OBJS) $(TARGET) $(TESTS)

debug: CFLAGS = $(COMMON_CFLAGS) -O0 -g
debug: clean all

asan: CC = clang
asan: CFLAGS = $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean all
