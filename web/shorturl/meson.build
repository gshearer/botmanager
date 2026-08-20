# shorturl — the URL shortener behind every short link botmanager mints.
#
# Two halves that never run on the same machine. botmanager's shorturl plugin
# WRITES rows (plugins/service/shorturl/); the daemon built here READS them,
# from the web host, over its own libpq connection. The table is the only thing
# between them, and src/{token,url,config}.* — compiled into both — are why the
# two agree on what a token and a target are.
#
# Guarded on purpose, the way senses/kokorod is: libfcgi is an operator-
# installed distro package, so a checkout without it must still build green.
# When it is absent this subdir declares no executable and the rest of the tree
# is unaffected — the plugin does not need it, and only the binaries do.

su_fcgi_dep = dependency('fcgi', required : false)
su_pq_dep   = dependency('libpq', version : '>= 9.5', required : false)  # ON CONFLICT

if su_fcgi_dep.found() and su_pq_dep.found()
  su_src_include = include_directories('src')

  # Shared by the daemon and the CLI. token.c and url.c appear here AND in the
  # plugin's target; separate compilation of the same source is what keeps one
  # definition serving both processes.
  su_core_lib = static_library('shorturl-core',
    'src/token.c',
    'src/url.c',
    'src/db.c',
    'src/http.c',
    dependencies        : [su_pq_dep, su_fcgi_dep],
    include_directories : su_src_include,
  )

  su_core_dep = declare_dependency(
    link_with           : su_core_lib,
    dependencies        : [su_pq_dep, su_fcgi_dep],
    include_directories : su_src_include,
  )

  # Only these two include version.h, which botmanager regenerates on every
  # ninja run — a TU that includes it recompiles every time.
  executable('shorturl.fcgi',
    'src/main.c',
    gen_version,
    dependencies        : su_core_dep,
    include_directories : gen_include,
    install             : false,
  )

  executable('shorturl',
    'src/admin.c',
    gen_version,
    dependencies        : su_core_dep,
    include_directories : gen_include,
    install             : false,
  )

else
  message('fcgi/libpq not found — skipping shorturl (install the fcgi and postgresql-libs packages to build it)')
endif
