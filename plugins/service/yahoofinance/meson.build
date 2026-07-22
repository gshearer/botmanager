yf_include = include_directories('.')

yahoofinance_plugin = shared_library('yahoofinance',
  'yahoofinance.c',
  include_directories : [inc_include, yf_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
