/*
 * md_ast.h — AST node types for Markdown parsing.
 *
 * Defines block and inline node types, allocation, tree construction,
 * and recursive deallocation. All strings are owned via strdup().
 */

#ifndef MD_AST_H
#define MD_AST_H

/* Block node types */
typedef enum {
    MD_BLOCK_DOCUMENT,
    MD_BLOCK_PARAGRAPH,
    MD_BLOCK_HEADING,
    MD_BLOCK_CODE_FENCE,
    MD_BLOCK_HRULE,
    MD_BLOCK_LIST,
    MD_BLOCK_LIST_ITEM,
    MD_BLOCK_TABLE,
    MD_BLOCK_TABLE_ROW,
    MD_BLOCK_TABLE_CELL,
    MD_BLOCK_BLOCKQUOTE
} md_block_type_t;

/* Inline node types */
typedef enum {
    MD_INLINE_TEXT,
    MD_INLINE_BOLD,
    MD_INLINE_ITALIC,
    MD_INLINE_BOLD_ITALIC,
    MD_INLINE_CODE,
    MD_INLINE_LINK,
    MD_INLINE_SOFTBREAK,
    MD_INLINE_HARDBREAK
} md_inline_type_t;

/* Table column alignment */
typedef enum {
    MD_ALIGN_LEFT   = 0,
    MD_ALIGN_CENTRE = 1,
    MD_ALIGN_RIGHT  = 2
} md_align_t;

/* Inline node */
typedef struct md_inline_node {
    md_inline_type_t type;
    char *text;                     /* Text content (TEXT, CODE) or NULL */
    char *url;                      /* Non-NULL only for LINK */
    struct md_inline_node *children; /* Child inlines (BOLD, ITALIC, LINK) */
    struct md_inline_node *next;    /* Sibling linked list */
} md_inline_node_t;

/* Block node */
typedef struct md_block_node {
    md_block_type_t type;
    int level;                      /* Heading level (1-4), list nesting depth */
    int ordered;                    /* List: 1=ordered, 0=unordered */
    int start;                      /* Ordered list start number */
    int is_header;                  /* Table row: 1=header, 0=body */
    char *language;                 /* CodeFence language tag or NULL */
    char *raw;                      /* CodeFence raw body text */
    int col_count;                  /* Table: number of columns */
    md_align_t *col_align;          /* Table: alignment per column */
    md_inline_node_t *inlines;      /* Inline content (linked list) */
    struct md_block_node *children; /* Nested blocks (linked list) */
    struct md_block_node *next;     /* Sibling linked list */
} md_block_node_t;

/* Create a new block node. Caller owns the returned pointer. */
md_block_node_t *md_block_create(md_block_type_t type);

/* Create a new inline node. Caller owns the returned pointer. */
md_inline_node_t *md_inline_create(md_inline_type_t type);

/* Create a text inline node with a copy of the given string. */
md_inline_node_t *md_inline_create_text(const char *text);

/* Create a code inline node with a copy of the given string. */
md_inline_node_t *md_inline_create_code(const char *text);

/* Create a link inline node with copies of URL and display text children. */
md_inline_node_t *md_inline_create_link(const char *url);

/* Append a child block to a parent block. */
void md_block_add_child(md_block_node_t *parent, md_block_node_t *child);

/* Append an inline node to a block's inline list. */
void md_block_add_inline(md_block_node_t *block, md_inline_node_t *inl);

/* Append a child inline to a parent inline (for nesting). */
void md_inline_add_child(md_inline_node_t *parent, md_inline_node_t *child);

/* Destroy a block node and ALL descendants recursively. */
void md_block_destroy(md_block_node_t *node);

/* Destroy an inline node and ALL descendants recursively. */
void md_inline_destroy(md_inline_node_t *node);

#endif /* MD_AST_H */
