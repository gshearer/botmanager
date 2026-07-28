strategy_kedge_plugin = shared_library(
  'strategy_kedge',
  'kedge.c',
  include_directories : [
    inc_include,
    whenmoon_include,
  ],
  dependencies : [m_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
