irc_plugin = shared_library('irc',
  'irc.c',
  'irc_commands.c',
  include_directories : [gen_include, inc_include],
  override_options : ['b_lundef=false'],
  install : true,
)
