oddsapi_include = include_directories('.')

# One .so: the service half, its parse layer and its own command surface.
# Legal because oddsapi is a dependency-graph leaf — see PLUGIN.md §Layer
# Rules, Rule 1.
oddsapi_plugin = shared_library('oddsapi',
  ['oddsapi.c', 'oddsapi_parse.c', 'oddsapi_cmd.c'],
  include_directories : [inc_include, oddsapi_include],
  dependencies : [json_c_dep, m_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
