strategy_fathom_plugin = shared_library(
  'strategy_fathom',
  'fathom.c',
  include_directories : [
    inc_include,
    whenmoon_include,
  ],
  dependencies : [m_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
