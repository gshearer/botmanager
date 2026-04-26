exchange_include = include_directories('.')

exchange_plugin = shared_library('exchange',
  'exchange.c',
  'exchange_registry.c',
  'exchange_limiter.c',
  'exchange_request.c',
  'exchange_retry.c',
  include_directories : [
    inc_include,
    exchange_include,
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
