wiki_plugin = shared_library('wiki',
  'wiki.c',
  'wiki_render.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    include_directories('../../service/wikimedia'),
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
