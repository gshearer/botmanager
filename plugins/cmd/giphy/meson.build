giphy_cmd_plugin = shared_library('giphy_cmd',
  'giphy_cmd.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    include_directories('../../service/giphy'),
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
