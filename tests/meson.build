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

test('util', test_util)
test('colors', test_colors)
test('json', test_json)
test('irc_parse', test_irc_parse)
test('fact_merge', test_fact_merge)
