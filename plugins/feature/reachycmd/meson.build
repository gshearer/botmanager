# The command surface over the reachyapi service. Separate .so from the
# service it wraps because reachyapi is not a dependency-graph leaf —
# the reachy protocol driver consumes it too, and must not inherit this
# plugin's upward bot_chat dependency.
reachycmd_plugin = shared_library('reachycmd',
  'reachycmd.c',
  include_directories : [inc_include, reachyapi_include, inference_include],
  override_options : ['b_lundef=false'],
  install : true,
)
