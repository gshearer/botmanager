sxng_include = include_directories('.')

searxng_plugin = shared_library('searxng',
  'searxng.c',
  include_directories : [inc_include, sxng_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
