giphy_include = include_directories('.')

# One .so: the service half plus its own command surface. Legal because
# giphy is a dependency-graph leaf — see PLUGIN.md §Layer Rules, Rule 1.
giphy_plugin = shared_library('giphy',
  ['giphy.c', 'giphy_cmd.c'],
  include_directories : [inc_include, giphy_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
