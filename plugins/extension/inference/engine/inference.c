// botmanager — MIT
// Inference plugin: descriptor + lifecycle glue over llm, knowledge,
// acquire. The subsystems' public APIs live in inference.h (dlsym-
// shim inlines); implementations live in the sibling .c files. This
// TU is inside the plugin, so it pulls in the three *_priv.h headers
// directly and the shim inlines in inference.h are suppressed by
// INFERENCE_INTERNAL.

#include "llm_priv.h"
#include "knowledge_priv.h"
#include "acquire_priv.h"

#include "common.h"
#include "clam.h"
#include "plugin.h"

#define INFERENCE_CTX  "inference"

static bool
inference_init(void)
{
  // db_init() and cmd_init() have already run from core main.c; the
  // three subsystems are free to register commands and issue DDL.
  llm_init();
  knowledge_init();
  acquire_init();

  llm_register_commands();
  knowledge_register_commands();
  acquire_register_commands();

  clam(CLAM_INFO, INFERENCE_CTX, "inference plugin initialized");
  return(SUCCESS);
}

static bool
inference_start(void)
{
  // kv_load() runs between plugin_init_all() and plugin_start_all().
  // Config-phase work (KV reads, CREATE TABLE DDL) belongs here so
  // KV defaults are visible and any operator overrides are applied.
  llm_register_config();
  knowledge_register_config();
  acquire_register_config();
  return(SUCCESS);
}

static bool
inference_stop(void)
{
  // No active drain today; present for symmetry.
  return(SUCCESS);
}

static void
inference_deinit(void)
{
  // Reverse init order.
  acquire_exit();
  knowledge_exit();
  llm_exit();
}

const plugin_desc_t bm_plugin_desc = {
  .api_version    = PLUGIN_API_VERSION,
  .name           = "inference",
  .version        = "1.0",
  .type           = PLUGIN_CORE,
  .kind           = "inference",
  .provides       = { { .name = "inference" } },
  .provides_count = 1,
  .requires       = {
    { .name = "core_kv"        },
    { .name = "core_db"        },
    { .name = "core_task"      },
    { .name = "core_cmd"       },
    { .name = "core_curl"      },
    { .name = "service_searxng"},
  },
  .requires_count = 6,
  .init           = inference_init,
  .start          = inference_start,
  .stop           = inference_stop,
  .deinit         = inference_deinit,
};
