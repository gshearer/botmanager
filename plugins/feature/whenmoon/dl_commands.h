// dl_commands.h — admin verbs for the whenmoon downloader.
// Internal; WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_DL_COMMANDS_H
#define BM_WHENMOON_DL_COMMANDS_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>
#include <stddef.h>

// Registers the downloader verbs under /whenmoon download (state-
// changing) and /show whenmoon download (observability). Invoked once
// from whenmoon_init.
bool wm_dl_register_verbs(void);

// Whitespace-delimited token extractor. Advances `*p` past leading
// whitespace and copies the run into `out` (capped). Returns true iff
// any character landed in `out`.
bool wm_dl_next_token(const char **p, char *out, size_t cap);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_DL_COMMANDS_H
