gem_include = include_directories('.')

gemini_plugin = shared_library('gemini',
  'gemini.c',
  'gemini_exchange.c',
  'gemini_orders.c',
  'gemini_pairs.c',
  'gemini_rest.c',
  'gemini_sign.c',
  'gemini_ws.c',
  'gemini_ws_channels.c',
  include_directories : [
    inc_include,
    gem_include,
    include_directories('../../feature/exchange'),
  ],
  dependencies : [json_c_dep, openssl_dep, curl_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
