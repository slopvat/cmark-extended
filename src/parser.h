/**
 * AI Disclosure:
 * - Authorship: AI-assisted
 * - Models: deepseek-v4.1-flash (DeepSeek)
 * - Scope: Added internal helpers for creating child nodes and advancing the
 *   parser offset.
 */

#ifndef CMARK_AST_H
#define CMARK_AST_H

#include <stdio.h>
#include "references.h"
#include "node.h"
#include "buffer.h"
#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LINK_LABEL_LENGTH 1000

struct cmark_parser {
  struct cmark_mem *mem;
  struct cmark_reference_map *refmap;
  struct cmark_node *root;
  struct cmark_node *current;
  int line_number;
  bufsize_t offset;
  bufsize_t column;
  bufsize_t first_nonspace;
  bufsize_t first_nonspace_column;
  bufsize_t thematic_break_kill_pos;
  int indent;
  bool blank;
  bool partially_consumed_tab;
  cmark_strbuf curline;
  bufsize_t last_line_length;
  cmark_strbuf linebuf;
  cmark_strbuf content;
  int options;
  bool last_buffer_ended_with_cr;
  unsigned int total_size;
};

cmark_node *cmark_parser_add_child(cmark_parser *parser, cmark_node *parent,
                                   cmark_node_type type, int start_column);

void cmark_parser_advance_offset(cmark_parser *parser, cmark_chunk *input,
                                 bufsize_t count, bool columns);

#ifdef __cplusplus
}
#endif

#endif
