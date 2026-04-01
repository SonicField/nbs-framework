/*
 * md_ast.c — AST node allocation, tree construction, and deallocation.
 *
 * All strings are owned by nodes via strdup(). md_block_destroy() frees
 * the entire tree recursively. No node references memory outside the tree.
 */

#define _POSIX_C_SOURCE 200809L

#include "md_ast.h"
#include "../nbs-common/nbs_assert.h"

#include <stdlib.h>
#include <string.h>

md_block_node_t *md_block_create(md_block_type_t type) {
    md_block_node_t *node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    node->type = type;
    return node;
}

md_inline_node_t *md_inline_create(md_inline_type_t type) {
    md_inline_node_t *node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    node->type = type;
    return node;
}

md_inline_node_t *md_inline_create_text(const char *text) {
    ASSERT_MSG(text != NULL, "md_inline_create_text: text is NULL");
    md_inline_node_t *node = md_inline_create(MD_INLINE_TEXT);
    if (!node) return NULL;
    node->text = strdup(text);
    if (!node->text) {
        free(node);
        return NULL;
    }
    return node;
}

md_inline_node_t *md_inline_create_code(const char *text) {
    ASSERT_MSG(text != NULL, "md_inline_create_code: text is NULL");
    md_inline_node_t *node = md_inline_create(MD_INLINE_CODE);
    if (!node) return NULL;
    node->text = strdup(text);
    if (!node->text) {
        free(node);
        return NULL;
    }
    return node;
}

md_inline_node_t *md_inline_create_link(const char *url) {
    ASSERT_MSG(url != NULL, "md_inline_create_link: url is NULL");
    md_inline_node_t *node = md_inline_create(MD_INLINE_LINK);
    if (!node) return NULL;
    node->url = strdup(url);
    if (!node->url) {
        free(node);
        return NULL;
    }
    return node;
}

void md_block_add_child(md_block_node_t *parent, md_block_node_t *child) {
    ASSERT_MSG(parent != NULL, "md_block_add_child: parent is NULL");
    ASSERT_MSG(child != NULL, "md_block_add_child: child is NULL");

    if (!parent->children) {
        parent->children = child;
    } else {
        md_block_node_t *tail = parent->children;
        while (tail->next) tail = tail->next;
        tail->next = child;
    }
}

void md_block_add_inline(md_block_node_t *block, md_inline_node_t *inl) {
    ASSERT_MSG(block != NULL, "md_block_add_inline: block is NULL");
    ASSERT_MSG(inl != NULL, "md_block_add_inline: inl is NULL");

    if (!block->inlines) {
        block->inlines = inl;
    } else {
        md_inline_node_t *tail = block->inlines;
        while (tail->next) tail = tail->next;
        tail->next = inl;
    }
}

void md_inline_add_child(md_inline_node_t *parent, md_inline_node_t *child) {
    ASSERT_MSG(parent != NULL, "md_inline_add_child: parent is NULL");
    ASSERT_MSG(child != NULL, "md_inline_add_child: child is NULL");

    if (!parent->children) {
        parent->children = child;
    } else {
        md_inline_node_t *tail = parent->children;
        while (tail->next) tail = tail->next;
        tail->next = child;
    }
}

void md_inline_destroy(md_inline_node_t *node) {
    while (node) {
        md_inline_node_t *next = node->next;
        free(node->text);
        free(node->url);
        md_inline_destroy(node->children);
        free(node);
        node = next;
    }
}

void md_block_destroy(md_block_node_t *node) {
    while (node) {
        md_block_node_t *next = node->next;
        free(node->language);
        free(node->raw);
        free(node->col_align);
        md_inline_destroy(node->inlines);
        md_block_destroy(node->children);
        free(node);
        node = next;
    }
}
