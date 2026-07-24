strategy_mako_plugin = shared_library(
  'strategy_mako',
  'mako.c',
  include_directories : [
    inc_include,
    whenmoon_include,
  ],
  dependencies : [m_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
