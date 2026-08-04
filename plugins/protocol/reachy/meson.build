# The Reachy Mini method driver. Consumes the reachyapi service through
# its dlsym shims and the inference engine through its own, so it links
# against neither — b_lundef=false is what lets those unresolved symbols
# stand until the loader satisfies them.
reachy_plugin = shared_library('reachy',
  'reachy.c',
  include_directories : [inc_include, reachyapi_include, inference_include],
  override_options : ['b_lundef=false'],
  install : true,
)
