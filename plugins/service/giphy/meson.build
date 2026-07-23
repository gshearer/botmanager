giphy_include = include_directories('.')

giphy_plugin = shared_library('giphy',
  'giphy.c',
  include_directories : [inc_include, giphy_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
