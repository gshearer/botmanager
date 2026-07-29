attack_plugin = shared_library('attack',
  'attack.c',
  'attack_db.c',
  'attack_combat.c',
  'attack_cmds.c',
  'attack_show.c',
  'attack_llm.c',
  'attack_dot.c',
  # inference_include exposes the engine's inference.h — the dlsym-shim
  # surface attack_llm.c gates behind plugin_find("inference").
  include_directories : [inc_include, inference_include],
  override_options : ['b_lundef=false'],
  install : true,
)
