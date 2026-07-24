tmdb_include = include_directories('.')

# One .so: the service half plus its own command surface. Legal because
# tmdb is a dependency-graph leaf — see PLUGIN.md §Layer Rules, Rule 1.
tmdb_plugin = shared_library('tmdb',
  ['tmdb.c', 'tmdb_cmd.c'],
  include_directories : [inc_include, tmdb_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
