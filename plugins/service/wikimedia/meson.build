wikimedia_include = include_directories('.')

wikimedia_plugin = shared_library('wikimedia',
  ['wikimedia.c', 'wikimedia_parse.c'],
  include_directories : [inc_include, wikimedia_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
