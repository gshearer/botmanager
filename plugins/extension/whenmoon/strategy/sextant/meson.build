strategy_sextant_plugin = shared_library(
  'strategy_sextant',
  'sextant.c',
  include_directories : [
    inc_include,
    whenmoon_include,
  ],
  dependencies : [m_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
