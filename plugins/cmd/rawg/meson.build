rawgcmd_plugin = shared_library('rawgcmd',
  'rawg_cmd.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    include_directories('../../service/rawg'),
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
