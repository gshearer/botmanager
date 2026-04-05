libpq = dependency('libpq', required : false)

if libpq.found()
  pg_inc = include_directories('.')

  # Plugin symbols (mem_alloc, db_result_set_*, etc.) are resolved at
  # runtime from the host executable. Disable undefined symbol check.
  pg_plugin = shared_library('pg',
    'pg.c',
    include_directories : [inc_include],
    dependencies : [libpq],
    override_options : ['b_lundef=false'],
    install : true,
  )
endif
