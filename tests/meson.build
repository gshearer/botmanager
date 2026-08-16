# Cases for the silent-wrongness surfaces only: a wrong answer here is
# a secret in a log, a fetch of a private address, forged formatting,
# or a person's fact quietly overwritten. Everything else in the tree
# fails loudly and recoverably and is deliberately untested (SC-TEST).
test_harness = static_library('test_harness',
  'test.c',
  include_directories : include_directories('.'),
)

# The two core suites link only what they call; libcore resolves the
# clam() and allocator symbols util.c and colors.c reach for.
test_util = executable('test_util',
  'test_util.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

test_colors = executable('test_colors',
  'test_colors.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

test_json = executable('test_json',
  'test_json.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# The IRC parser is a plugin translation unit, so this suite compiles
# that one file directly — irc_parse.c calls nothing but clam() and libc,
# which is why it is a file of its own.
test_irc_parse = executable('test_irc_parse',
  'test_irc_parse.c',
  '../plugins/method/irc/irc_parse.c',
  include_directories : [include_directories('.'), inc_include, gen_include,
                         include_directories('../plugins/method/irc')],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# The DB facade decides things no engine ever sees — which statement it
# declined to send, which verb it sent instead. This suite drives it
# through a driver that runs no SQL and only records, so those decisions
# are what the cases read. No database is involved.
test_db_txn = executable('test_db_txn',
  'test_db_txn.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# The cooldown ring is a plugin translation unit that calls nothing but
# libc, so this suite compiles that one file directly and links no core.
test_cooldown = executable('test_cooldown',
  'test_cooldown.c',
  '../plugins/bot/chat/cooldown.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories('../plugins/bot/chat')],
  link_with : [test_harness],
)

# The FACT-1 ladder is SQL, so its suite drives psql against the
# configured database inside a rolled-back transaction, and SKIPs when
# that database is unreachable. It needs no core symbols — only the
# statement, which memory.h carries.
test_fact_merge = executable('test_fact_merge',
  'test_fact_merge.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories('../plugins/bot/chat')],
  link_with : [test_harness],
)

# The per-bot key accessors compose a key for ~110 call sites. Their one
# silent failure is a key too long to compose reading as the shorter key
# it truncates onto, so this suite drives kv.c's own table through the
# public accessors — kv_init() is DB-free, and nothing here flushes.
test_kv_bot = executable('test_kv_bot',
  'test_kv_bot.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# kv_get_str hands ~205 sites a pointer they use after the lock is
# gone. Both ways that promise can break are silent — a value rewritten
# under a reader, an entry freed under one — so this suite states the
# lifetime as pointer identity, which is red without a sanitizer.
test_kv_str = executable('test_kv_str',
  'test_kv_str.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# The backpressure submit is the one call in the tree that parks a
# caller on a condition another host controls. Its bound is the whole
# protection and its failure is silent — a parked thread logs nothing —
# so this suite states the bound as elapsed milliseconds. It runs the
# real subsystem with the drain disabled (core.curl.max_active = 0), so
# no case touches the network.
test_curl_submit_wait = executable('test_curl_submit_wait',
  'test_curl_submit_wait.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

test('util', test_util)
test('colors', test_colors)
test('json', test_json)
test('irc_parse', test_irc_parse)
test('db_txn', test_db_txn)
test('cooldown', test_cooldown)
test('fact_merge', test_fact_merge)
test('kv_bot', test_kv_bot)
test('kv_str', test_kv_str)
test('curl_submit_wait', test_curl_submit_wait)
