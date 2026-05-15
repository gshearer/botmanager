// botmanager — MIT
// Gemini WebSocket channel multiplexer — empty stub.
//
// GEM-3 lands the real multiplexer alongside the transport. The
// lifecycle hooks below are no-ops so gem_init / gem_deinit have a
// stable call surface.
#define GEM_INTERNAL
#include "gemini.h"

void
gem_ws_channels_init(void)
{
  clam(CLAM_DEBUG, GEM_CTX, "ws_channels: init stub (GEM-3 lands here)");
}

void
gem_ws_channels_deinit(void)
{
  clam(CLAM_DEBUG, GEM_CTX, "ws_channels: deinit stub (GEM-3 lands here)");
}
