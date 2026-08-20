su_include = include_directories('.')

# The daemon's directory, NOT its src/ — see the include note in shorturl.h.
su_daemon_include = include_directories('../../../web/shorturl')

# token.c and url.c are the trust boundary, and they are compiled from the
# daemon's directory rather than copied here on purpose: the alphabet that
# mints a token and the alphabet that parses one must be the same code, and
# so must SHORTURL_TARGET_MAX and the CHECK constraint generated from it.
shorturl_plugin = shared_library('shorturl',
  'shorturl.c',
  '../../../web/shorturl/src/token.c',
  '../../../web/shorturl/src/url.c',
  include_directories : [inc_include, su_include, su_daemon_include],
  override_options : ['b_lundef=false'],
  install : true,
)
