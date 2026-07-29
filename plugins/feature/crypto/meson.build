crypto_plugin = shared_library('crypto',
  'crypto.c',
  'crypto_lists.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    include_directories('../../service/coinmarketcap'),
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
