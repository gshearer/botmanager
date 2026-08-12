chat_plugin = shared_library('chat',
  ['chatbot.c', 'dispatch.c', 'personality.c', 'personality_show.c',
   'contract_show.c', 'commands.c',
   'dossier.c', 'dossier_cmd.c', 'dossier_show.c',
   'identity.c',
   'speak_policy.c', 'nl_bridge.c', 'nl_observe.c', 'interpret.c',
   'soul.c', 'deferred.c', 'voice.c', 'occasions.c', 'followups.c',
   'reply.c',
   'show_verbs.c', 'interests.c', 'memory.c', 'memory_rag.c',
   'memory_cmd.c', 'memory_backfill.c',
   'volunteer.c', 'vision.c', 'extract.c', 'extract_prompt.c'],
  # '.' first so plugin-local headers (memory.h, dossier.h, dispatch.h)
  # win over any older include/ copy during transitional builds. The
  # inference include dir exposes the engine's inference.h — the
  # dlsym-shim public surface for llm/knowledge/acquire (defined by
  # plugins/extension/inference/engine/meson.build). The weathergov
  # dir exposes the CAP alert types the soul's weather watch consumes
  # (resolution is tolerant plugin_dlsym, never the abort-shims).
  include_directories : [include_directories('.'), inc_include,
                         inference_include,
                         include_directories('../../service/weathergov')],
  override_options : ['b_lundef=false'],
  install : true,
)
