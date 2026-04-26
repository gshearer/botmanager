cb_include = include_directories('.')

coinbase_plugin = shared_library('coinbase',
  'coinbase.c',
  'coinbase_exchange.c',
  'coinbase_orders.c',
  'coinbase_rest.c',
  'coinbase_sign.c',
  'coinbase_ws.c',
  'coinbase_ws_channels.c',
  include_directories : [
    inc_include,
    cb_include,
    include_directories('../../feature/exchange'),
  ],
  dependencies : [json_c_dep, openssl_dep, curl_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
