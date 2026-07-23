rawg_include = include_directories('.')

rawg_plugin = shared_library('rawg',
  'rawg.c',
  include_directories : [inc_include, rawg_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
