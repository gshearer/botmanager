math_plugin = shared_library('math',
  'math.c',
  meson.project_source_root() / 'core' / 'math_expr.c',
  include_directories : [inc_include],
  dependencies : [m_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
