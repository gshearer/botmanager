ow_include = include_directories('.')

openweather_plugin = shared_library('openweather',
  'openweather.c',
  include_directories : [inc_include, ow_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
