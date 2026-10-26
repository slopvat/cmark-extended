/**
 * AI Disclosure:
 * - Authorship: AI-generated
 * - Models: deepseek-v4.1-flash (DeepSeek)
 * - Scope: Added the extended autolink postprocessing entry point.
 */

#ifndef CMARK_AUTOLINK_H
#define CMARK_AUTOLINK_H

#include "cmark.h"

#ifdef __cplusplus
extern "C" {
#endif

void cmark_autolink_process(cmark_mem *mem, cmark_node *root);

#ifdef __cplusplus
}
#endif

#endif
