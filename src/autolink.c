/**
 * AI Disclosure:
 * - Authorship: AI-generated
 * - Models: deepseek-v4.1-flash (DeepSeek)
 * - Scope: Ported the extended autolink postprocessor from cmark-gfm; links
 *   for www., URL schemes and email addresses are created without parsing
 *   options.
 * - References: https://github.com/github/cmark-gfm (27d942c)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "autolink.h"
#include "cmark.h"
#include "cmark_ctype.h"
#include "node.h"
#include "utf8.h"

static bool S_is_alnum(unsigned char c) {
  return cmark_isalpha((char)c) || cmark_isdigit((char)c);
}

static bool S_is_valid_hostchar(const uint8_t *link, size_t link_len) {
  int32_t ch;
  int r = cmark_utf8proc_iterate(link, (bufsize_t)link_len, &ch);

  if (r < 0)
    return false;
  return !cmark_utf8proc_is_space(ch) &&
         !cmark_utf8proc_is_punctuation(ch);
}

static bool S_strcaseeq(const uint8_t *str, const char *scheme, size_t len) {
  size_t i;

  for (i = 0; i < len; i++) {
    unsigned char a = str[i];
    unsigned char b = (unsigned char)scheme[i];

    if (a >= 'A' && a <= 'Z')
      a = (unsigned char)(a - 'A' + 'a');
    if (a != b)
      return false;
  }
  return true;
}

static size_t S_autolink_delim(const uint8_t *data, size_t link_end) {
  size_t i;
  size_t closing = 0;
  size_t opening = 0;

  for (i = 0; i < link_end; ++i) {
    const uint8_t c = data[i];

    if (c == '<') {
      link_end = i;
      break;
    } else if (c == '(') {
      opening++;
    } else if (c == ')') {
      closing++;
    }
  }

  while (link_end > 0) {
    switch (data[link_end - 1]) {
    case ')':
      if (closing <= opening)
        return link_end;
      closing--;
      link_end--;
      break;
    case '?':
    case '!':
    case '.':
    case ',':
    case ':':
    case '*':
    case '_':
    case '~':
    case '\'':
    case '"':
      link_end--;
      break;
    case ';': {
      size_t new_end = link_end - 2;

      while (new_end > 0 && cmark_isalpha((char)data[new_end]))
        new_end--;

      if (new_end < link_end - 2 && data[new_end] == '&')
        link_end = new_end;
      else
        link_end--;
      break;
    }

    default:
      return link_end;
    }
  }

  return link_end;
}

static size_t S_check_domain(const uint8_t *data, size_t size,
                             bool allow_short) {
  size_t i, np = 0, uscore1 = 0, uscore2 = 0;

  if (size < 2) {
    if (allow_short && size == 1)
      return 1;
    return 0;
  }

  for (i = 1; i < size - 1; i++) {
    if (data[i] == '\\' && i < size - 2)
      i++;
    if (data[i] == '_')
      uscore2++;
    else if (data[i] == '.') {
      uscore1 = uscore2;
      uscore2 = 0;
      np++;
    } else if (!S_is_valid_hostchar(data + i, size - i) && data[i] != '-')
      break;
  }

  if (uscore1 > 0 || uscore2 > 0) {
    /* Accept very long URLs despite the underscores to avoid quadratic
     * behavior. */
    if (np <= 10)
      return 0;
  }

  if (allow_short)
    return i;

  return np ? i : 0;
}

static bool S_match_scheme(const uint8_t *data, size_t size, size_t colon,
                           size_t *start, size_t *end) {
  static const char *schemes[] = {"http", "https", "ftp"};
  size_t scheme_start = colon;
  size_t i, domain_len, link_end;
  bool found = false;

  if (colon + 2 >= size || data[colon + 1] != '/' || data[colon + 2] != '/')
    return false;

  while (scheme_start > 0 && cmark_isalpha((char)data[scheme_start - 1]))
    scheme_start--;

  for (i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
    size_t len = strlen(schemes[i]);

    if (colon - scheme_start == len &&
        S_strcaseeq(data + scheme_start, schemes[i], len)) {
      found = true;
      break;
    }
  }
  if (!found)
    return false;

  if (!S_is_valid_hostchar(data + colon + 3, size - colon - 3))
    return false;

  domain_len = S_check_domain(data + colon + 3, size - colon - 3, true);
  if (domain_len == 0)
    return false;

  link_end = 3 + domain_len;
  while (colon + link_end < size &&
         !cmark_isspace((char)data[colon + link_end]) &&
         data[colon + link_end] != '<')
    link_end++;

  link_end = S_autolink_delim(data + colon, link_end);
  if (link_end == 0)
    return false;

  *start = scheme_start;
  *end = colon + link_end;
  return true;
}

static bool S_match_www(const uint8_t *data, size_t size, size_t pos,
                        size_t *start, size_t *end) {
  size_t domain_len, link_end;

  if (pos > 0) {
    unsigned char prev = data[pos - 1];

    if (strchr("*_~(", prev) == NULL && !cmark_isspace((char)prev))
      return false;
  }

  if (size - pos < 4 || memcmp(data + pos, "www.", 4) != 0)
    return false;

  domain_len = S_check_domain(data + pos, size - pos, false);
  if (domain_len == 0)
    return false;

  link_end = domain_len;
  while (pos + link_end < size &&
         !cmark_isspace((char)data[pos + link_end]) &&
         data[pos + link_end] != '<')
    link_end++;

  link_end = S_autolink_delim(data + pos, link_end);
  if (link_end == 0)
    return false;

  *start = pos;
  *end = pos + link_end;
  return true;
}

static bool S_validate_protocol(const char *protocol, const uint8_t *at,
                                size_t rewind, size_t max_rewind) {
  size_t len = strlen(protocol);

  if (len > max_rewind - rewind)
    return false;

  if (memcmp(at - rewind - len, protocol, len) != 0)
    return false;

  if (len == max_rewind - rewind)
    return true;

  return !S_is_alnum(at[-((ptrdiff_t)(rewind + len + 1))]);
}

static bool S_match_email(const uint8_t *data, size_t size, size_t segment,
                          size_t at, size_t *start, size_t *end,
                          const char **url_prefix) {
  size_t max_rewind = at - segment;
  size_t rewind, link_end, np = 0;
  bool auto_mailto = true;
  bool is_xmpp = false;

  for (rewind = 0; rewind < max_rewind; ++rewind) {
    uint8_t c = data[at - rewind - 1];

    if (S_is_alnum(c))
      continue;

    if (strchr(".+-_", c) != NULL)
      continue;

    if (c == ':') {
      if (S_validate_protocol("mailto:", data + at, rewind, max_rewind)) {
        auto_mailto = false;
        continue;
      }

      if (S_validate_protocol("xmpp:", data + at, rewind, max_rewind)) {
        auto_mailto = false;
        is_xmpp = true;
        continue;
      }
    }

    break;
  }

  if (rewind == 0)
    return false;

  for (link_end = 1; link_end < size - at; ++link_end) {
    uint8_t c = data[at + link_end];

    if (S_is_alnum(c))
      continue;

    if (c == '@')
      return false;

    if (c == '.' && link_end < size - at - 1 &&
        S_is_alnum(data[at + link_end + 1]))
      np++;
    else if (c == '/' && is_xmpp)
      continue;
    else if (c != '-' && c != '_')
      break;
  }

  if (link_end < 2 || np == 0 ||
      (!cmark_isalpha((char)data[at + link_end - 1]) &&
       data[at + link_end - 1] != '.'))
    return false;

  link_end = S_autolink_delim(data + at, link_end);
  if (link_end == 0)
    return false;

  *start = at - rewind;
  *end = at + link_end;
  *url_prefix = (auto_mailto && !is_xmpp) ? "mailto:" : "";
  return true;
}

static void S_set_literal(cmark_mem *mem, cmark_node *node,
                          const uint8_t *data, size_t len) {
  uint8_t *buf = (uint8_t *)mem->realloc(NULL, len + 1);

  if (len)
    memcpy(buf, data, len);
  buf[len] = 0;

  mem->free(node->data);
  node->data = buf;
  node->len = (bufsize_t)len;
}

typedef struct {
  size_t start;
  size_t end;
  const char *url_prefix;
} autolink_match;

static cmark_node *S_new_link(cmark_mem *mem, const uint8_t *data, size_t start,
                              size_t end, const char *url_prefix,
                              int start_column, int line) {
  cmark_node *link = cmark_node_new_with_mem(CMARK_NODE_LINK, mem);
  cmark_node *link_text = cmark_node_new_with_mem(CMARK_NODE_TEXT, mem);
  cmark_strbuf url = CMARK_BUF_INIT(mem);
  size_t link_len = end - start;

  if (url_prefix)
    cmark_strbuf_puts(&url, url_prefix);
  cmark_strbuf_put(&url, data + start, (bufsize_t)link_len);

  link->as.link.url = cmark_strbuf_detach(&url);
  link->as.link.title = NULL;
  link->start_line = link->end_line = line;
  link->start_column = start_column + (int)start;
  link->end_column = start_column + (int)end - 1;

  S_set_literal(mem, link_text, data + start, link_len);
  link_text->start_line = link_text->end_line = line;
  link_text->start_column = link->start_column;
  link_text->end_column = link->end_column;
  cmark_node_append_child(link, link_text);

  return link;
}

static cmark_node *S_new_text(cmark_mem *mem, const uint8_t *data, size_t start,
                              size_t end, int start_column, int line) {
  cmark_node *node = cmark_node_new_with_mem(CMARK_NODE_TEXT, mem);

  S_set_literal(mem, node, data + start, end - start);
  node->start_line = node->end_line = line;
  node->start_column = start_column + (int)start;
  node->end_column = start_column + (int)end - 1;

  return node;
}

static void S_update_depth(const uint8_t *data, size_t *pos, int *depth,
                           size_t target) {
  while (*pos < target) {
    if (data[*pos] == '[')
      (*depth)++;
    else if (data[*pos] == ']' && *depth > 0)
      (*depth)--;
    (*pos)++;
  }
}

static void S_postprocess_text(cmark_mem *mem, cmark_node *text, int *depth) {
  size_t total = (size_t)text->len;
  uint8_t *buffer;
  autolink_match *matches = NULL;
  size_t n_matches = 0;
  size_t matches_cap = 0;
  size_t segment_start = 0;
  size_t segment_len = total;
  size_t scan_from = 0;
  size_t brack_pos = 0;
  int brack_depth = *depth;
  int start_column = text->start_column;
  int line = text->start_line;
  cmark_node *prev = text;
  size_t cursor = 0;
  size_t i;

  if (total == 0)
    return;

  buffer = (uint8_t *)mem->realloc(NULL, total + 1);
  memcpy(buffer, text->data, total);
  buffer[total] = 0;

  while (scan_from < segment_len) {
    size_t j, start = 0, end = 0;
    const char *url_prefix = NULL;
    bool found = false;
    bool suppressed = false;

    for (j = scan_from; j < segment_len; j++) {
      size_t abs = segment_start + j;
      uint8_t c;

      S_update_depth(buffer, &brack_pos, &brack_depth, abs);
      c = buffer[abs];

      if (c == '@') {
        if (S_match_email(buffer, segment_start + segment_len, segment_start,
                          abs, &start, &end, &url_prefix)) {
          found = true;
          break;
        }
      } else if (c == ':') {
        if (S_match_scheme(buffer, segment_start + segment_len, abs, &start,
                           &end)) {
          found = true;
          suppressed = brack_depth > 0;
          break;
        }
      } else if (c == 'w' && abs + 4 <= segment_start + segment_len &&
                 memcmp(buffer + abs, "www.", 4) == 0) {
        if (S_match_www(buffer, segment_start + segment_len, abs, &start,
                        &end)) {
          url_prefix = "http://";
          found = true;
          suppressed = brack_depth > 0;
          break;
        }
      }
    }

    if (!found)
      break;

    if (suppressed) {
      scan_from = j + 1;
      continue;
    }

    if (n_matches == matches_cap) {
      matches_cap = matches_cap ? matches_cap * 2 : 8;
      matches = (autolink_match *)mem->realloc(
          matches, matches_cap * sizeof(autolink_match));
    }
    matches[n_matches].start = start;
    matches[n_matches].end = end;
    matches[n_matches].url_prefix = url_prefix;
    n_matches++;

    brack_pos = end;
    segment_len = (segment_start + segment_len) - end;
    segment_start = end;
    scan_from = 0;
  }

  S_update_depth(buffer, &brack_pos, &brack_depth, total);
  *depth = brack_depth;

  if (n_matches == 0) {
    mem->free(buffer);
    mem->free(matches);
    return;
  }

  for (i = 0; i < n_matches; i++) {
    cmark_node *link;

    if (i == 0) {
      S_set_literal(mem, text, buffer + cursor, matches[i].start - cursor);
      text->start_column = start_column + (int)cursor;
      text->end_column = start_column + (int)matches[i].start - 1;
    } else if (matches[i].start > cursor) {
      cmark_node *gap = S_new_text(mem, buffer, cursor, matches[i].start,
                                   start_column, line);

      cmark_node_insert_after(prev, gap);
      prev = gap;
    }

    link = S_new_link(mem, buffer, matches[i].start, matches[i].end,
                      matches[i].url_prefix, start_column, line);
    cmark_node_insert_after(prev, link);
    prev = link;
    cursor = matches[i].end;
  }

  if (cursor < total) {
    cmark_node *tail =
        S_new_text(mem, buffer, cursor, total, start_column, line);

    cmark_node_insert_after(prev, tail);
  }

  mem->free(matches);
  mem->free(buffer);
}

static void S_process_inlines(cmark_mem *mem, cmark_node *parent, int *depth) {
  cmark_node *child = parent->first_child;

  while (child) {
    cmark_node *next = child->next;

    switch (child->type) {
    case CMARK_NODE_TEXT:
      S_postprocess_text(mem, child, depth);
      break;
    case CMARK_NODE_EMPH:
    case CMARK_NODE_STRONG:
    case CMARK_NODE_STRIKETHROUGH:
      S_process_inlines(mem, child, depth);
      break;
    case CMARK_NODE_IMAGE: {
      int image_depth = *depth + 1;

      S_process_inlines(mem, child, &image_depth);
      break;
    }
    default:
      break;
    }

    child = next;
  }
}

static void S_process_block(cmark_mem *mem, cmark_node *node) {
  cmark_node *child;

  if (node->type == CMARK_NODE_PARAGRAPH || node->type == CMARK_NODE_HEADING ||
      node->type == CMARK_NODE_TABLE_CELL) {
    int depth = 0;

    S_process_inlines(mem, node, &depth);
    return;
  }

  for (child = node->first_child; child; child = child->next)
    S_process_block(mem, child);
}

void cmark_autolink_process(cmark_mem *mem, cmark_node *root) {
  if (root == NULL)
    return;

  S_process_block(mem, root);
}
