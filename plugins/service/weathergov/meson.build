wxg_include = include_directories('.')

weathergov_plugin = shared_library('weathergov',
  'weathergov.c',
  include_directories : [inc_include, wxg_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
