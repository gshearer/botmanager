imagine_cmd_plugin = shared_library('imagine_cmd',
  'imagine_cmd.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    inference_include,
  ],
  dependencies : [uuid_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
