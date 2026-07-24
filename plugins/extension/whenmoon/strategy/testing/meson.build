strategy_testing_plugin = shared_library(
  'strategy_testing',
  'testing.c',
  include_directories : [
    inc_include,
    whenmoon_include,
  ],
  dependencies : [m_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
