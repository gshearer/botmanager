wordnik_include = include_directories('.')

# One .so: the service half plus its own command surface. Legal because
# wordnik is a dependency-graph leaf — see PLUGIN.md §Layer Rules, Rule 1.
wordnik_plugin = shared_library('wordnik',
  ['wordnik.c', 'wordnik_cmd.c'],
  include_directories : [inc_include, wordnik_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
