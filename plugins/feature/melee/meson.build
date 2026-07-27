melee_plugin = shared_library('melee',
  'melee.c',
  'melee_db.c',
  'melee_combat.c',
  'melee_cmds.c',
  'melee_show.c',
  'melee_llm.c',
  'melee_dot.c',
  # inference_include exposes the engine's inference.h — the dlsym-shim
  # surface melee_llm.c gates behind plugin_find("inference").
  include_directories : [inc_include, inference_include],
  override_options : ['b_lundef=false'],
  install : true,
)
