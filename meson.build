project('botmanager', 'c',
  version : '0.1',
  default_options : [
    'c_std=gnu11',
    'warning_level=3',
    'optimization=2',
  ],
)

# C99 zero-inits unspecified fields in designated initialisers. The
# project uses that idiom heavily (json_spec_t tables, plugin_kv_entry_t
# schemas, cmd_arg_desc_t specs) — silence the cosmetic warning.
add_project_arguments('-Wno-missing-field-initializers', language : 'c')

# The clean-build set is -Wall -Wextra -Wpedantic -Wformat-security; the first
# three are warning_level=3. -Wformat-security is in none of them: a printf(s)
# whose s came from outside is silent under all three and under FORTIFY.
add_project_arguments('-Wformat-security', language : 'c')

# Release hardening. -D_FORTIFY_SOURCE=3 needs -O1 or better and warns when it
# does not have it, so it is set per configuration rather than globally; the
# other two cost nothing at any optimization level and are unconditional.
add_project_arguments('-fstack-protector-strong', '-fstack-clash-protection',
  language : 'c')

# ...and a sanitizer build is not a release build. FORTIFY redirects the very
# libc entry points ASan replaces with its own instrumented ones, so leaving it
# on there trades a report that names the overflow for a bare __chk abort that
# does not. The gate is silent either way, which is the reason to state it here.
if get_option('optimization') != '0' and get_option('b_sanitize') == 'none'
  add_project_arguments('-D_FORTIFY_SOURCE=3', language : 'c')
endif

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

# libxml2 for RSS/Atom + HTML parsing (inference plugin: acquire feeds).
xml2_dep = dependency('libxml-2.0', required : true)

# TA-Lib (BSD-3) for technical indicators (whenmoon plugin). System-
# packaged on Arch via the AUR `ta-lib` package; pkg-config exposes
# the include path and the libta-lib.so.0 link line. We do NOT vendor;
# operators install the dep before building.
talib_dep = dependency('ta-lib', required : true)


subdir('core')
subdir('plugins')
subdir('tools')
# Out-of-daemon speech hosts for the reachy robot. Not plugins and not linked
# against core — kokorod is a standalone daemon. Self-guarding on sherpa-onnx.
subdir('senses/kokorod')
