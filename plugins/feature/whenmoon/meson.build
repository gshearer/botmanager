candle_upsample_sql_h = custom_target(
  'candle_upsample.sql.h',
  input   : 'sql/candle_upsample.sql',
  output  : 'candle_upsample.sql.h',
  command : ['xxd', '-i',
             '-n', 'wm_dl_candle_upsample_ddl',
             '@INPUT@', '@OUTPUT@'],
)

# Exported for strategy plugins (PLUGIN_STRATEGY) so they can find
# whenmoon_strategy.h — the public ABI header that publishes the
# bar / grain / indicator types and the dlsym shim helpers.
whenmoon_include = include_directories('.')

whenmoon_plugin = shared_library('whenmoon',
  'whenmoon.c',
  'market.c',
  'account.c',
  'aggregator.c',
  'indicators.c',
  'book_persist.c',
  'dl_schema.c',
  'dl_coverage.c',
  'dl_jobtable.c',
  'dl_supervisor.c',
  'dl_candles.c',
  'cd_probe.c',
  'dl_commands.c',
  'market_cmds.c',
  'strategy.c',
  'strategy_cmds.c',
  'pnl.c',
  'sizer.c',
  'order.c',
  'trade_cmds.c',
  'backtest.c',
  'backtest_cmds.c',
  'sweep.c',
  candle_upsample_sql_h,
  include_directories : [
    inc_include,
    whenmoon_include,
    include_directories('../../service/coinbase'),
    include_directories('../exchange'),
  ],
  dependencies : [
    talib_dep,
    json_c_dep,
  ],
  override_options : ['b_lundef=false'],
  install : true,
)

install_data(
  'sql/candle_upsample.sql',
  install_dir : 'share/botmanager/whenmoon',
)
