/**
 * AI Disclosure:
 * - Authorship: AI-generated
 * - Models: deepseek-v4.1-flash (DeepSeek)
 * - Scope: Ported the table block parser, row scanners and cell handling from
 *   cmark-gfm to cmark's parser internals.
 * - References: https://github.com/github/cmark-gfm (27d942c)
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cmark.h"
#include "cmark_ctype.h"
#include "chunk.h"
#include "inlines.h"
#include "node.h"
#include "parser.h"
#include "table.h"

typedef struct {
  cmark_strbuf *buf;
  int start_offset, end_offset, internal_offset;
} node_cell;

typedef struct {
  uint16_t n_columns;
  int paragraph_offset;
  node_cell *cells;
} table_row;

static bool S_is_spacechar(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

static bool S_is_escaped_char(unsigned char c) {
  switch (c) {
  case '|':
  case '!':
  case '"':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '(':
  case ')':
  case '*':
  case '+':
  case ',':
  case '.':
  case '/':
  case ':':
  case ';':
  case '<':
  case '=':
  case '>':
  case '?':
  case '@':
  case '[':
  case '\\':
  case ']':
  case '^':
  case '_':
  case '`':
  case '{':
  case '}':
  case '~':
  case '-':
    return true;
  default:
    return false;
  }
}

/* Scans a delimiter cell, returning the position after it or 0. */
static bufsize_t S_scan_table_marker(const unsigned char *data, bufsize_t len,
                                     bufsize_t pos) {
  bufsize_t start = pos;

  while (pos < len && S_is_spacechar(data[pos]))
    pos++;
  if (pos < len && data[pos] == ':')
    pos++;
  if (pos >= len || data[pos] != '-')
    return 0;
  while (pos < len && data[pos] == '-')
    pos++;
  if (pos < len && data[pos] == ':')
    pos++;
  while (pos < len && S_is_spacechar(data[pos]))
    pos++;

  return pos > start ? pos : 0;
}

/* Scans a table delimiter row, returning its length or 0. */
static bufsize_t S_scan_table_start(cmark_chunk *input, bufsize_t offset) {
  const unsigned char *data = input->data;
  bufsize_t len = input->len;
  bufsize_t pos = offset;

  if (pos < len && data[pos] == '|')
    pos++;

  pos = S_scan_table_marker(data, len, pos);
  if (pos == 0)
    return 0;

  for (;;) {
    bufsize_t next;

    if (pos >= len || data[pos] != '|')
      break;
    next = S_scan_table_marker(data, len, pos + 1);
    if (next == 0)
      break;
    pos = next;
  }

  if (pos < len && data[pos] == '|')
    pos++;

  while (pos < len && S_is_spacechar(data[pos]))
    pos++;

  if (pos < len && data[pos] == '\r')
    pos++;
  if (pos < len && data[pos] == '\n')
    return pos + 1 - offset;

  return 0;
}

static bufsize_t S_scan_table_cell(const unsigned char *data, bufsize_t len,
                                   bufsize_t pos) {
  bufsize_t start = pos;

  while (pos < len) {
    unsigned char c = data[pos];

    if (c == '\\' && pos + 1 < len && S_is_escaped_char(data[pos + 1])) {
      pos += 2;
    } else if (c != '|' && c != '\r' && c != '\n') {
      pos++;
    } else {
      break;
    }
  }

  return pos - start;
}

static bufsize_t S_scan_table_cell_end(const unsigned char *data,
                                       bufsize_t len, bufsize_t pos) {
  bufsize_t start = pos;

  if (pos < len && data[pos] == '|') {
    pos++;
    while (pos < len && S_is_spacechar(data[pos]))
      pos++;
  }

  return pos - start;
}

static bufsize_t S_scan_table_row_end(const unsigned char *data, bufsize_t len,
                                      bufsize_t pos) {
  bufsize_t start = pos;

  while (pos < len && S_is_spacechar(data[pos]))
    pos++;
  if (pos < len && data[pos] == '\r')
    pos++;
  if (pos < len && data[pos] == '\n')
    return pos + 1 - start;

  return 0;
}

static cmark_strbuf *S_unescape_pipes(cmark_mem *mem,
                                      const unsigned char *string,
                                      bufsize_t len) {
  cmark_strbuf *res = (cmark_strbuf *)mem->calloc(1, sizeof(cmark_strbuf));
  bufsize_t r, w;

  cmark_strbuf_init(mem, res, len + 1);
  cmark_strbuf_put(res, string, len);
  cmark_strbuf_putc(res, '\0');

  for (r = 0, w = 0; r < len; ++r) {
    if (res->ptr[r] == '\\' && r + 1 < len && res->ptr[r + 1] == '|')
      r++;

    res->ptr[w++] = res->ptr[r];
  }

  cmark_strbuf_truncate(res, w);

  return res;
}

static void S_free_table_cell(cmark_mem *mem, node_cell *cell) {
  cmark_strbuf_free(cell->buf);
  mem->free(cell->buf);
}

static void S_free_row_cells(cmark_mem *mem, table_row *row) {
  while (row->n_columns > 0) {
    S_free_table_cell(mem, &row->cells[--row->n_columns]);
  }
  mem->free(row->cells);
  row->cells = NULL;
}

static void S_free_table_row(cmark_mem *mem, table_row *row) {
  if (!row)
    return;

  S_free_row_cells(mem, row);
  mem->free(row);
}

static node_cell *S_append_row_cell(cmark_mem *mem, table_row *row) {
  const uint32_t n_columns = row->n_columns + 1;

  /* realloc when n_columns is a power of 2 */
  if ((n_columns & (n_columns - 1)) == 0) {
    /* make sure we never wrap row->n_columns */
    if (n_columns > UINT16_MAX)
      return NULL;
    row->cells = (node_cell *)mem->realloc(
        row->cells, (2 * n_columns - 1) * sizeof(node_cell));
  }
  row->n_columns = (uint16_t)n_columns;
  return &row->cells[n_columns - 1];
}

/* Parses a single table row of the form
 * `delim? table_cell (delim table_cell)* delim? newline`.
 */
static table_row *S_row_from_string(cmark_mem *mem,
                                    const unsigned char *string, int len) {
  table_row *row;
  bufsize_t cell_matched = 1, pipe_matched = 1, offset;
  int expect_more_cells = 1;
  int row_end_offset = 0;
  int int_overflow_abort = 0;

  row = (table_row *)mem->calloc(1, sizeof(table_row));
  row->n_columns = 0;
  row->cells = NULL;

  offset = S_scan_table_cell_end(string, len, 0);

  while (offset < len && expect_more_cells) {
    cell_matched = S_scan_table_cell(string, len, offset);
    pipe_matched = S_scan_table_cell_end(string, len, offset + cell_matched);

    if (cell_matched || pipe_matched) {
      cmark_strbuf *cell_buf =
          S_unescape_pipes(mem, string + offset, cell_matched);
      node_cell *cell;

      cmark_strbuf_trim(cell_buf);

      cell = S_append_row_cell(mem, row);
      if (!cell) {
        int_overflow_abort = 1;
        cmark_strbuf_free(cell_buf);
        mem->free(cell_buf);
        break;
      }
      cell->buf = cell_buf;
      cell->start_offset = offset;
      cell->end_offset = offset + cell_matched - 1;
      cell->internal_offset = 0;

      while (cell->start_offset > row->paragraph_offset &&
             string[cell->start_offset - 1] != '|') {
        --cell->start_offset;
        ++cell->internal_offset;
      }
    }

    offset += cell_matched + pipe_matched;

    if (pipe_matched) {
      expect_more_cells = 1;
    } else {
      row_end_offset = S_scan_table_row_end(string, len, offset);
      offset += row_end_offset;

      if (row_end_offset && offset != len) {
        row->paragraph_offset = offset;

        S_free_row_cells(mem, row);

        offset += S_scan_table_cell_end(string, len, offset);

        expect_more_cells = 1;
      } else {
        expect_more_cells = 0;
      }
    }
  }

  if (offset != len || row->n_columns == 0 || int_overflow_abort) {
    S_free_table_row(mem, row);
    row = NULL;
  }

  return row;
}

static int S_get_n_autocompleted_cells(cmark_node *table) {
  const cmark_table *t = &table->as.table;

  return ((int)t->n_columns * t->n_rows) - t->n_nonempty_cells;
}

static void S_set_content(cmark_node *node, cmark_strbuf *buf) {
  node->len = buf->size;
  node->data = cmark_strbuf_detach(buf);
}

static bool S_is_blank(const cmark_strbuf *s, bufsize_t offset) {
  while (offset < s->size) {
    switch (s->ptr[offset]) {
    case '\r':
    case '\n':
      return true;
    case ' ':
    case '\t':
      offset++;
      break;
    default:
      return false;
    }
  }

  return true;
}

static bool S_resolve_reference_definitions(cmark_parser *parser,
                                            cmark_strbuf *content) {
  cmark_chunk chunk = {content->ptr, content->size};
  bufsize_t pos;

  while (chunk.len && chunk.data[0] == '[' &&
         (pos = cmark_parse_reference_inline(parser->mem, &chunk,
                                             parser->refmap))) {
    chunk.data += pos;
    chunk.len -= pos;
  }
  cmark_strbuf_drop(content, content->size - chunk.len);
  return !S_is_blank(content, 0);
}

static void S_insert_header_paragraph(cmark_parser *parser,
                                      cmark_node *paragraph,
                                      cmark_node *table,
                                      const unsigned char *content,
                                      int paragraph_offset) {
  cmark_strbuf *prefix =
      S_unescape_pipes(parser->mem, content, paragraph_offset);
  cmark_node *new_paragraph;
  bufsize_t pos;
  int n_lines = 0;
  bufsize_t last_line_len = 0;

  if (!S_resolve_reference_definitions(parser, prefix)) {
    cmark_strbuf_free(prefix);
    parser->mem->free(prefix);
    return;
  }

  cmark_strbuf_trim(prefix);

  new_paragraph =
      cmark_node_new_with_mem(CMARK_NODE_PARAGRAPH, parser->mem);
  S_set_content(new_paragraph, prefix);
  parser->mem->free(prefix);

  for (pos = 0; pos < new_paragraph->len; pos++) {
    if (new_paragraph->data[pos] == '\n') {
      n_lines++;
      last_line_len = 0;
    } else if (new_paragraph->data[pos] != '\r') {
      last_line_len++;
    }
  }

  new_paragraph->start_line = paragraph->start_line;
  new_paragraph->start_column = paragraph->start_column;
  new_paragraph->end_line =
      paragraph->start_line + n_lines - (last_line_len == 0 ? 1 : 0);
  new_paragraph->end_column = last_line_len;

  if (!cmark_node_insert_before(table, new_paragraph)) {
    cmark_node_free(new_paragraph);
  }
}

bool cmark_table_row_matches(cmark_parser *parser, cmark_node *table,
                             cmark_chunk *input) {
  table_row *row;
  bool res;

  (void)table;

  row = S_row_from_string(parser->mem,
                          input->data + parser->first_nonspace,
                          (int)(input->len - parser->first_nonspace));
  res = row != NULL && row->n_columns > 0;
  S_free_table_row(parser->mem, row);

  return res;
}

bool cmark_table_open_header(cmark_parser *parser, cmark_node **container,
                             cmark_chunk *input) {
  cmark_node *paragraph = *container;
  table_row *header_row;
  table_row *delimiter_row;
  cmark_node *header;
  uint8_t *alignments;
  int i;

  if (paragraph->flags & CMARK_NODE__TABLE_CHECKED)
    return false;

  if (!S_scan_table_start(input, parser->first_nonspace))
    return false;

  delimiter_row = S_row_from_string(
      parser->mem, input->data + parser->first_nonspace,
      (int)(input->len - parser->first_nonspace));
  if (!delimiter_row)
    return false;

  header_row = S_row_from_string(parser->mem, parser->content.ptr,
                                 (int)parser->content.size);
  if (!header_row || header_row->n_columns != delimiter_row->n_columns) {
    paragraph->flags |= CMARK_NODE__TABLE_CHECKED;
    S_free_table_row(parser->mem, header_row);
    S_free_table_row(parser->mem, delimiter_row);
    return false;
  }

  if (header_row->paragraph_offset) {
    S_insert_header_paragraph(parser, paragraph, paragraph, parser->content.ptr,
                              header_row->paragraph_offset);
  }

  alignments = (uint8_t *)parser->mem->calloc(delimiter_row->n_columns,
                                              sizeof(uint8_t));
  for (i = 0; i < delimiter_row->n_columns; ++i) {
    node_cell *cell = &delimiter_row->cells[i];
    bool left = cell->buf->size > 0 && cell->buf->ptr[0] == ':';
    bool right =
        cell->buf->size > 0 && cell->buf->ptr[cell->buf->size - 1] == ':';

    if (left && right)
      alignments[i] = 'c';
    else if (left)
      alignments[i] = 'l';
    else if (right)
      alignments[i] = 'r';
  }

  paragraph->type = (uint16_t)CMARK_NODE_TABLE;
  paragraph->as.table.n_columns = header_row->n_columns;
  paragraph->as.table.n_rows = 0;
  paragraph->as.table.n_nonempty_cells = 0;
  paragraph->as.table.alignments = alignments;

  header = cmark_parser_add_child(parser, paragraph, CMARK_NODE_TABLE_ROW,
                                  paragraph->start_column);
  header->as.table_row.is_header = true;
  header->end_column =
      paragraph->start_column + (int)parser->content.size - 2;
  header->start_line = header->end_line = paragraph->start_line;

  for (i = 0; i < header_row->n_columns; ++i) {
    node_cell *cell = &header_row->cells[i];
    cmark_node *cell_node = cmark_parser_add_child(
        parser, header, CMARK_NODE_TABLE_CELL,
        paragraph->start_column + cell->start_offset);

    cell_node->start_line = cell_node->end_line = paragraph->start_line;
    cell_node->as.table_cell.internal_offset = cell->internal_offset;
    cell_node->as.table_cell.cell_index = (uint16_t)i;
    cell_node->end_column = paragraph->start_column + cell->end_offset;
    S_set_content(cell_node, cell->buf);
  }

  paragraph->as.table.n_rows = 1;
  paragraph->as.table.n_nonempty_cells = i;

  cmark_strbuf_clear(&parser->content);
  cmark_parser_advance_offset(parser, input,
                              input->len - 1 - parser->offset, false);

  S_free_table_row(parser->mem, header_row);
  S_free_table_row(parser->mem, delimiter_row);

  *container = paragraph;
  return true;
}

bool cmark_table_open_row(cmark_parser *parser, cmark_node **container,
                          cmark_chunk *input) {
  cmark_node *table = *container;
  cmark_node *row;
  table_row *parsed;
  int n_columns = (int)table->as.table.n_columns;
  int i;

  if (parser->blank)
    return false;

  if (S_get_n_autocompleted_cells(table) >
      CMARK_TABLE_MAX_AUTOCOMPLETED_CELLS)
    return false;

  parsed = S_row_from_string(parser->mem,
                             input->data + parser->first_nonspace,
                             (int)(input->len - parser->first_nonspace));
  if (!parsed)
    return false;

  row = cmark_parser_add_child(parser, table, CMARK_NODE_TABLE_ROW,
                               table->start_column);
  row->end_column = table->end_column;

  for (i = 0; i < parsed->n_columns && i < n_columns; ++i) {
    node_cell *cell = &parsed->cells[i];
    cmark_node *cell_node = cmark_parser_add_child(
        parser, row, CMARK_NODE_TABLE_CELL,
        table->start_column + cell->start_offset);

    cell_node->start_line = cell_node->end_line = parser->line_number;
    cell_node->as.table_cell.internal_offset = cell->internal_offset;
    cell_node->as.table_cell.cell_index = (uint16_t)i;
    cell_node->end_column = table->start_column + cell->end_offset;
    S_set_content(cell_node, cell->buf);
  }

  table->as.table.n_rows++;
  table->as.table.n_nonempty_cells += i;

  for (; i < n_columns; ++i) {
    cmark_node *cell_node =
        cmark_parser_add_child(parser, row, CMARK_NODE_TABLE_CELL, 0);

    cell_node->start_line = cell_node->end_line = parser->line_number;
    cell_node->as.table_cell.cell_index = (uint16_t)i;
  }

  cmark_parser_advance_offset(parser, input,
                              input->len - 1 - parser->offset, false);

  S_free_table_row(parser->mem, parsed);

  *container = row;
  return true;
}
