/**
 * AI Disclosure:
 * - Authorship: AI-generated
 * - Models: deepseek-v4.1-flash (DeepSeek)
 * - Scope: Ported the table parser interface from cmark-gfm.
 */

#ifndef CMARK_TABLE_H
#define CMARK_TABLE_H

#include <stdbool.h>

#include "cmark.h"
#include "chunk.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Limit the number of cells created by row autocompletion to avoid
 * denial-of-service inputs. */
#define CMARK_TABLE_MAX_AUTOCOMPLETED_CELLS 0x80000

bool cmark_table_row_matches(cmark_parser *parser, cmark_node *table,
                             cmark_chunk *input);

bool cmark_table_open_header(cmark_parser *parser, cmark_node **container,
                             cmark_chunk *input);

bool cmark_table_open_row(cmark_parser *parser, cmark_node **container,
                          cmark_chunk *input);

#ifdef __cplusplus
}
#endif

#endif
