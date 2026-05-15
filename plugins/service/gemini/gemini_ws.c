// botmanager — MIT
// Gemini WebSocket transport — empty stub.
//
// GEM-3 lands the real two-session reader (Market Data v2 + Order
// Events). The lifecycle hooks below are intentional no-ops so the
// linker is satisfied and gem_init / gem_start / gem_stop /
// gem_deinit have a stable call surface across all GEM-* chunks.
#define GEM_INTERNAL
#include "gemini.h"

void
gem_ws_init(void)
{
  clam(CLAM_DEBUG, GEM_CTX, "ws: init stub (GEM-3 lands here)");
}

void
gem_ws_start(void)
{
  clam(CLAM_DEBUG, GEM_CTX, "ws: start stub (GEM-3 lands here)");
}

void
gem_ws_stop(void)
{
  clam(CLAM_DEBUG, GEM_CTX, "ws: stop stub (GEM-3 lands here)");
}

void
gem_ws_deinit(void)
{
  clam(CLAM_DEBUG, GEM_CTX, "ws: deinit stub (GEM-3 lands here)");
}
