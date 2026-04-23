whenmoon_plugin = shared_library('whenmoon',
  'whenmoon.c',
  'market.c',
  'account.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    include_directories('../../service/coinbase'),
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
