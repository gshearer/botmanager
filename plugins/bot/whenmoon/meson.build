candle_upsample_sql_h = custom_target(
  'candle_upsample.sql.h',
  input   : 'sql/candle_upsample.sql',
  output  : 'candle_upsample.sql.h',
  command : ['xxd', '-i',
             '-n', 'wm_dl_candle_upsample_ddl',
             '@INPUT@', '@OUTPUT@'],
)

whenmoon_plugin = shared_library('whenmoon',
  'whenmoon.c',
  'market.c',
  'account.c',
  'aggregator.c',
  'indicators.c',
  'dl_schema.c',
  'dl_coverage.c',
  'dl_scheduler.c',
  'dl_trades.c',
  'dl_candles.c',
  'dl_commands.c',
  'market_cmds.c',
  candle_upsample_sql_h,
  include_directories : [
    inc_include,
    include_directories('.'),
    include_directories('../../service/coinbase'),
  ],
  dependencies : [
    talib_dep,
  ],
  override_options : ['b_lundef=false'],
  install : true,
)

install_data(
  'sql/candle_upsample.sql',
  install_dir : 'share/botmanager/whenmoon',
)
