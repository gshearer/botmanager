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

# Six renderers grew their own copy of this column arithmetic and the
# copies had started to disagree (OBS-8). One copy, one set of cases.
test_display = executable('test_display',
  'test_display.c',
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

# The chat plugin's drain for the async records core cannot see. A
# shutdown that returns early is silent until a reload lands on a
# running callback months later, which is exactly the silent-wrongness
# bar this directory is filtered to. hold.c includes no chat header —
# that is the property the whole scheme rests on — so this suite
# compiles the one file directly; inference_include is for the dlsym
# shim it calls llm_cancel_user through, and libcore resolves clam()
# and curl_request_cancel.
test_hold = executable('test_hold',
  'test_hold.c',
  '../plugins/bot/chat/hold.c',
  include_directories : [include_directories('.'), inc_include,
                         inference_include,
                         include_directories('../plugins/bot/chat')],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# whenmoon's barrier for a deferred task that re-arms itself. A drain
# that returns while a body is still inside the plugin is a dlclose over
# live code and says nothing at the time — OBS-43 crashed a reload
# months after the chain was written. warm_chain.c includes no whenmoon
# header (that is the property the scheme rests on), so this suite
# compiles the one file directly; libcore is for the REAL task queues,
# and since no worker pool is started here a deferred sits in the timer
# queue and nothing runs it — exactly the determinism a drain test wants.
test_warm_chain = executable('test_warm_chain',
  'test_warm_chain.c',
  '../plugins/extension/whenmoon/warm_chain.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories('../plugins/extension/whenmoon')],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# The one predicate that decides whether a live WS subscription may be
# left alone (OBS-41). Both wrong answers are silent: "same" when the set
# changed leaves a market with no feed, and "differs" when it did not
# restores the ~17 s tick gap the diff exists to end. ws_binding.c
# includes no whenmoon header — the property that lets this suite compile
# it directly, as warm_chain.c does — and calls nothing but libc, so no
# core is linked.
test_ws_binding = executable('test_ws_binding',
  'test_ws_binding.c',
  '../plugins/extension/whenmoon/ws_binding.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories('../plugins/extension/whenmoon')],
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

# Ingest idempotency (OBS-16) is one SQL statement and one unique index,
# so the only honest test of it runs them — same shape as fact_merge:
# psql inside a rolled-back transaction over TEMP tables that shadow the
# real ones, SKIP when the database is unreachable. The statement comes
# from knowledge_priv.h so it cannot drift from the one production
# sends, and that header is all this needs — no plugin symbols, hence
# no libcore.
test_knowledge_dedup = executable('test_knowledge_dedup',
  'test_knowledge_dedup.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories(
                             '../plugins/extension/inference/engine')],
  link_with : [test_harness],
)

# The per-bot key accessors compose a key for ~110 call sites. Their one
# silent failure is a key too long to compose reading as the shorter key
# it truncates onto, so this suite drives kv.c's own table through the
# public accessors — kv_init() is DB-free, and nothing here flushes.
# `set bot` decides which real KV key a write lands on, and that pick has
# been wrong twice (OBS-54). A wrong form is a silent write to an
# adjacent registered knob, so the parse is a pure function and this is
# its table. No bot subsystem, no KV store, no daemon.
test_set_bot = executable('test_set_bot',
  'test_set_bot.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories('../core')],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

test_kv_bot = executable('test_kv_bot',
  'test_kv_bot.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# 28 read sites wrote a knob's default a second time, downstream of the
# schema that already declared it (OBS-13). The pairs agreed, so what
# these cases hold is the property that made them agree by hand: a
# declaration is what the read answers with.
test_kv_default = executable('test_kv_default',
  'test_kv_default.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# kv_reset's correctness is a negative — the entry must be left CLEAN —
# and the flush that would expose it is the same flush the command
# issues on success, so a revert routed through apply_val re-persists the
# row it just dropped and says nothing at the time. These cases install a
# db driver that runs no SQL and records what the facade asked of it, so
# the statements are what they read; no database is involved.
test_kv_reset = executable('test_kv_reset',
  'test_kv_reset.c',
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

# A plugin's stop() proves the negative core cannot: that no completion
# callback of its own is running or ever will be. The failure mode is
# silent in both directions — a drain that returns early lets deinit()
# tear down under a live callback, and a drain that never returns turns
# an unload into a hang — so this suite states both halves in elapsed
# milliseconds. Same fixture as curl_submit_wait: the real subsystem
# with core.curl.max_active = 0, so no case touches the network.
test_curl_flight = executable('test_curl_flight',
  'test_curl_flight.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# The quiescence barrier asks cmd_inflight_owned() whether a command
# handler is running inside the mapping it is about to unmap. A wrong
# answer has no error path: the unload proceeds and the handler resumes
# at an address that is no longer code — measured as a SIGSEGV whose
# fault site was inside libc. A mapping is just an address range, so
# these cases need no dlopen; they drive cmd_invoke() and let a handler
# ask about its own address.
test_cmd_inflight = executable('test_cmd_inflight',
  'test_cmd_inflight.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# The kraken WS multiplexer names a slot by its index in two places that
# outlive the lock hold which made the index true — across the reconcile
# emit window and across the ack round trip — while compaction reshuffles
# by swap-with-last. Every wrong answer is silent: fanout reads the
# subscriber list and never the slot table, so the damage lands on what
# the next reconcile decides to emit — a feed that never subscribes, an
# unsubscribe that never goes out. These cases drive the public surface
# only; the send stub is the interleaving control point.
# Gemini's slot table carries the same subject as kraken's — what the
# table BELIEVES the gateway holds against what it actually holds — but
# arrives at it through four divergences kraken does not have, because
# Gemini MD v2 echoes no req_id and so has no correlator ring. Every
# consequence is silent: fan-out never reads slot state, so a wrong
# belief corrupts only what the NEXT pass emits (OBS-42).
test_gemini_ws_channels = executable('test_gemini_ws_channels',
  'test_gemini_ws_channels.c',
  '../plugins/service/gemini/gemini_ws_channels.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories('../plugins/service/gemini'),
                         include_directories('../plugins/feature/exchange')],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

test_coinbase_ws_channels = executable('test_coinbase_ws_channels',
  'test_coinbase_ws_channels.c',
  '../plugins/service/coinbase/coinbase_ws_channels.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories('../plugins/service/coinbase'),
                         include_directories('../plugins/feature/exchange')],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

test_kraken_ws_channels = executable('test_kraken_ws_channels',
  'test_kraken_ws_channels.c',
  '../plugins/service/kraken/kraken_ws_channels.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories('../plugins/service/kraken'),
                         include_directories('../plugins/feature/exchange')],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# A protocol plugin's deinit frees every WS subscriber node it issued,
# and the registry entry naming that plugin survives the unload — so
# after a reload the consumer's handle resolves to a live exchange again
# and a freed pointer reaches the revived driver. The second free has no
# error path: it is a [FATAL] on a stderr that is /dev/null, or, once the
# address has been reused, an innocent block released in silence. These
# cases drive the abstraction's public surface with a driver stub whose
# teardown frees what it issued, which is the whole mechanism.
test_exchange_ws_gen = executable('test_exchange_ws_gen',
  'test_exchange_ws_gen.c',
  '../plugins/feature/exchange/exchange_caps.c',
  '../plugins/feature/exchange/exchange_registry.c',
  '../plugins/feature/exchange/exchange_limiter.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories('../plugins/feature/exchange')],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# Dropping a stale handle safely is not the same as coming back: after a
# provider reload the revived registration owns an empty slot table, and
# a consumer whose state is keyed to that registration is never told —
# whenmoon's market feed streamed nothing until an unrelated market
# command happened to rebuild it. The registration watch is that
# telling. These cases hold the contract the consumer leans on: fires on
# SUCCESS only, fresh and revived alike, and with no lock held, so the
# callback may subscribe again from inside the fire.
test_exchange_watch = executable('test_exchange_watch',
  'test_exchange_watch.c',
  '../plugins/feature/exchange/exchange_caps.c',
  '../plugins/feature/exchange/exchange_registry.c',
  '../plugins/feature/exchange/exchange_limiter.c',
  include_directories : [include_directories('.'), inc_include,
                         include_directories('../plugins/feature/exchange')],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# The method traffic counters are written by whatever thread replies —
# method_send holds no lock — and read under method_mutex into
# /show methods. A lost increment is silent and permanent, so these
# rows state the contract as exact arithmetic over the public surface:
# red under a plain -O2 build (lost updates) and red under
# -fsanitize=thread, with no daemon and no network.
test_method_stats = executable('test_method_stats',
  'test_method_stats.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# The control socket serves several sessions at once and an async verb
# answers long after the dispatch that asked for it returned. Handing
# that answer to whoever the driver is serving at that instant is silent
# wrongness across a session boundary — one operator's output printed
# into another's terminal, on a path that runs with the credential
# context open. These cases drive the real driver over real AF_UNIX
# clients with two test verbs: one that answers from a pool worker, one
# that holds a dispatch window open while it does.
test_botmanctl_route = executable('test_botmanctl_route',
  'test_botmanctl_route.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

# The shutdown reason is operator text that ends up on a wire where the
# line ending IS the message boundary — an IRC QUIT. A CR or LF that
# survived the copy would close that QUIT and deliver everything after
# it as a further command from an owner-level bot, and nothing on the
# path would report it. These cases hold the boundary that folds them.
test_shutdown_reason = executable('test_shutdown_reason',
  'test_shutdown_reason.c',
  include_directories : [include_directories('.'), inc_include],
  link_with : [test_harness, libcore],
  dependencies : core_deps,
)

test('util', test_util)
test('colors', test_colors)
test('display', test_display)
test('json', test_json)
test('irc_parse', test_irc_parse)
test('db_txn', test_db_txn)
test('cooldown', test_cooldown)
test('hold', test_hold)
test('warm_chain', test_warm_chain)
test('fact_merge', test_fact_merge)
test('knowledge_dedup', test_knowledge_dedup)
test('kv_bot', test_kv_bot)
test('set_bot', test_set_bot)
test('kv_str', test_kv_str)
test('kv_default', test_kv_default)
test('kv_reset', test_kv_reset)
test('curl_submit_wait', test_curl_submit_wait)
test('curl_flight', test_curl_flight)
test('cmd_inflight', test_cmd_inflight)
test('kraken_ws_channels', test_kraken_ws_channels)
test('gemini_ws_channels', test_gemini_ws_channels)
test('coinbase_ws_channels', test_coinbase_ws_channels)
test('exchange_ws_gen', test_exchange_ws_gen)
test('exchange_watch', test_exchange_watch)
test('method_stats', test_method_stats)
test('botmanctl_route', test_botmanctl_route)
test('shutdown_reason', test_shutdown_reason)
test('ws_binding', test_ws_binding)
