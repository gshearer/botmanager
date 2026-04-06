cmc_include = include_directories('.')

coinmarketcap_plugin = shared_library('coinmarketcap',
  'coinmarketcap.c',
  include_directories : [inc_include, cmc_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
