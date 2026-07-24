strategy_example_sma_cross_plugin = shared_library(
  'strategy_example_sma_cross',
  'example_sma_cross.c',
  include_directories : [
    inc_include,
    whenmoon_include,
  ],
  dependencies : [m_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
