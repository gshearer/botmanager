tmdbcmd_plugin = shared_library('tmdbcmd',
  'tmdb_cmd.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    include_directories('../../service/tmdb'),
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
