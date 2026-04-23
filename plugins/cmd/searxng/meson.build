searxng_cmd_plugin = shared_library('searxng_cmd',
  'searxng_cmd.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    include_directories('../../service/searxng'),
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
