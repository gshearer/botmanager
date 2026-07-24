rawg_include = include_directories('.')

# One .so: the service half plus its own command surface. Legal because
# rawg is a dependency-graph leaf — see PLUGIN.md §Layer Rules, Rule 1.
rawg_plugin = shared_library('rawg',
  ['rawg.c', 'rawg_cmd.c'],
  include_directories : [inc_include, rawg_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
