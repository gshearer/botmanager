# Sources for the core static library (everything except main.c).
libcore_sources = [
  'admin.c',
  'admin_bot.c',
  'admin_help.c',
  'admin_user.c',
  'bconf.c',
  'bot.c',
  'botmanctl.c',
  'clam.c',
  'colors.c',
  'cmd.c',
  'console.c',
  'curl.c',
  'db.c',
  'kv.c',
  'mem.c',
  'method.c',
  'plugin.c',
  'pool.c',
  'resolve.c',
  'sig.c',
  'sock.c',
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
core_deps = [dl_dep, argon2_dep, openssl_dep, curl_dep, resolv_dep, uuid_dep, m_dep, readline_dep]

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
  link_with : [libcore],
  dependencies : core_deps,
  export_dynamic : true,
  install : true,
)
