weather_plugin = shared_library('weather',
  'weather.c',
  'weather_render.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    include_directories('../../service/openweather'),
    include_directories('../../service/weathergov'),
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
