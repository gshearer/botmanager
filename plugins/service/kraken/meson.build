kr_include = include_directories('.')

kraken_plugin = shared_library('kraken',
  'kraken.c',
  'kraken_exchange.c',
  'kraken_orders.c',
  'kraken_pairs.c',
  'kraken_persist.c',
  'kraken_rest.c',
  'kraken_sign.c',
  'kraken_ws.c',
  'kraken_ws_channels.c',
  include_directories : [
    inc_include,
    kr_include,
    include_directories('../../feature/exchange'),
  ],
  dependencies : [json_c_dep, openssl_dep, curl_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
