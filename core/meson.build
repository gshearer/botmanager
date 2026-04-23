# Sources for the core static library (everything except main.c).
libcore_sources = [
  'bot_cmd.c',
  'cmd_misc.c',
  'cmd_set.c',
  'cmd_show.c',
  'userns_cmd.c',
  'bconf.c',
  'bot.c',
  'botmanctl.c',
  'clam.c',
  'clam_cmd.c',
  'colors.c',
  'cmd.c',
  'curl.c',
  'db.c',
  'json.c',
  'kv.c',
  'alloc.c',
  'method.c',
  'plugin.c',
  'proc.c',
  'pool.c',
  'resolve.c',
  'sig.c',
  'sock.c',
  'sse.c',
  'task.c',
  'userns.c',
  'userns_cache.c',
  'userns_mfa.c',
  'userns_util.c',
  'util.c',
  'validate.c',
  'math_expr.c',
]

# All external dependencies needed by core modules.
core_deps = [dl_dep, argon2_dep, openssl_dep, curl_dep, resolv_dep, uuid_dep, m_dep, json_c_dep]

# Static library used by both the main binary and all tests.
libcore = static_library('core',
  libcore_sources,
  gen_version,
  include_directories : [gen_include, inc_include],
  dependencies : core_deps,
)

# Main executable: links the static library + main.c.
botman = executable('botman',
  'main.c',
  gen_version,
  include_directories : [gen_include, inc_include],
  link_whole : [libcore],
  dependencies : core_deps,
  export_dynamic : true,
  install : true,
)
