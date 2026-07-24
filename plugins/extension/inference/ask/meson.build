ask_cmd_plugin = shared_library('ask_cmd',
  'ask_cmd.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    inference_include,
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
