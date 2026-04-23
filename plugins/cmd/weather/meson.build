weather_plugin = shared_library('weather',
  'weather.c',
  include_directories : [
    inc_include,
    include_directories('.'),
    include_directories('../../service/openweather'),
  ],
  override_options : ['b_lundef=false'],
  install : true,
)
