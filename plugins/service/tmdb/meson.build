tmdb_include = include_directories('.')

tmdb_plugin = shared_library('tmdb',
  'tmdb.c',
  include_directories : [inc_include, tmdb_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
