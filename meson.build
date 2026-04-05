project('botmanager', 'c',
  version : '0.1',
  default_options : [
    'c_std=gnu11',
    'warning_level=3',
    'optimization=2',
  ],
)

bm_version_major = '0'
bm_version_minor = '1'

# Generate version.h with auto-incrementing build number.
# This runs on every build, ensuring the build number always advances.
gen_version = custom_target('version_h',
  output : 'version.h',
  command : [
    find_program('scripts/gen_version_h.sh'),
    meson.project_source_root() / 'BUILDNUM',
    bm_version_major,
    bm_version_minor,
    '@OUTPUT@',
  ],
  build_always_stale : true,
)

# The generated version.h lands in the build directory.
# This include dir makes it available to all targets.
gen_include = include_directories('.')

# Public headers used by core and plugins.
inc_include = include_directories('include')

# libdl for plugin loading (dlopen/dlclose).
cc = meson.get_compiler('c')
dl_dep = cc.find_library('dl', required : false)

# libargon2 for password hashing (user authentication).
argon2_dep = dependency('libargon2', required : true)

# OpenSSL for TLS support (socket service).
openssl_dep = dependency('openssl', required : true)

# libcurl for HTTP client (curl subsystem).
curl_dep = dependency('libcurl', required : true)

# json-c for JSON parsing (service plugins).
json_c_dep = dependency('json-c', required : true)

# libresolv for DNS resolution (resolver subsystem).
resolv_dep = cc.find_library('resolv', required : true)

# libuuid for UUID generation (user namespace).
uuid_dep = dependency('uuid', required : true)

# libm for math functions (math_expr core module).
m_dep = cc.find_library('m', required : false)

# GNU Readline for console line editing and history.
readline_dep = dependency('readline', required : true)

subdir('core')
subdir('plugins')
subdir('tools')
subdir('tests')
