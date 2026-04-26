// dl_commands.h — admin verbs for the whenmoon downloader.
// Internal; WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_DL_COMMANDS_H
#define BM_WHENMOON_DL_COMMANDS_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>
#include <stddef.h>

// Registers the four admin verbs + their two parent slots:
//   /bot <name> download <trades|cancel>
//   /show bot <name> download <status|gaps>
// Invoked once from whenmoon_init after whenmoon_register_show_verbs.
bool wm_dl_register_verbs(void);

// Shared parsers — reused by the market verbs. Token extraction
// advances `*p` past the tokenised run; pair parsing splits
// "exch:base:quote" into components + a "BASE-QUOTE" coinbase-style
// symbol (uppercased). SUCCESS on success.
bool wm_dl_next_token(const char **p, char *out, size_t cap);

bool wm_dl_parse_pair(const char *in,
    char *exch,   size_t exch_cap,
    char *base,   size_t base_cap,
    char *quote,  size_t quote_cap,
    char *symbol, size_t sym_cap);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_DL_COMMANDS_H
