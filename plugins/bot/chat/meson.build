chat_plugin = shared_library('chat',
  ['chatbot.c', 'personality.c', 'personality_show.c',
   'contract_show.c', 'commands.c',
   'dossier.c', 'dossier_cmd.c', 'dossier_show.c',
   'identity.c',
   'speak_policy.c', 'nl_bridge.c', 'nl_observe.c', 'reply.c',
   'show_verbs.c', 'interests.c', 'memory.c', 'memory_rag.c',
   'memory_cmd.c', 'user_show.c',
   'volunteer.c', 'vision.c', 'extract.c', 'extract_prompt.c'],
  # '.' first so chat-plugin-local headers (memory.h, dossier.h) win
  # over any older include/ copy during transitional builds. The
  # inference include dir exposes plugins/inference/inference.h (the
  # dlsym-shim public surface for llm/knowledge/acquire).
  include_directories : [include_directories('.'), inc_include,
                         inference_include,
                         plugin_contracts_include],
  override_options : ['b_lundef=false'],
  install : true,
)
