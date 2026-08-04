reachyapi_include = include_directories('.')

# Mechanism only: no command surface lives here. reachyapi is a non-leaf
# service (the reachycmd command plugin and the reachy protocol driver
# both consume it), so PLUGIN.md §Layer Rules Rule 1 forbids it holding
# commands of its own.
reachyapi_plugin = shared_library('reachyapi',
  ['reachyapi.c'],
  include_directories : [inc_include, reachyapi_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
