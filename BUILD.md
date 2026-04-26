# BUILD.md — Build, Test, and Install

## Prerequisites

System packages (Arch names; equivalent on other distros):
- `meson`, `ninja`
- `gcc` (or clang) supporting `-std=gnu11`
- `libargon2`, `openssl`, `libcurl`, `json-c`, `libresolv`, `libuuid`,
  `libdl`, `libm`, `libxml2`
- `ta-lib` (BSD-3 technical-indicator library — AUR on Arch:
  `yay -S ta-lib`. Used by the whenmoon plugin only; meson reports
  `Run-time dependency ta-lib found: YES <ver>` at setup.)

DB driver (currently the only one, runtime-loadable plugin):
- `postgresql` client library (`libpq`)

## Build

```sh
meson setup build
ninja -C build
```

Both commands run from the project root (`/mnt/fast/doc/projects/botmanager`).
Re-running `meson setup build` is unnecessary after the first time —
`ninja -C build` picks up `meson.build` changes automatically.

The build is expected to be **warning-free for files you touched**. A few
pre-existing `-Wmissing-field-initializers` and `-Wformat-truncation`
warnings exist in `plugins/service/coinmarketcap` and `plugins/method/irc`;
don't introduce new warnings.

`scripts/gen_version_h.sh` runs every build and bumps the `BUILDNUM` file
at the project root.

## Test

The test suite has been removed. New tests will be reintroduced in a
future pass; for now, verify changes by building and manually exercising
the running daemon via `botmanctl` and `ircspyctl`.

## Outputs

| Path | What |
|------|------|
| `build/core/botman` | Main daemon binary |
| `build/tools/botmanctl`, `build/tools/ircspy`, `build/tools/ircspyctl` | CLI tools — see `tools/AGENTS.md` |
| `build/plugins/<type>/<name>/lib<name>.so` | Runtime-loadable plugins |
| `build/version.h` | Generated each build |

## Install

There is no `meson install` workflow yet. Run binaries from `build/`
directly. Daemon expects `~/.config/botmanager/botman.conf` for its DB
bootstrap (sample format: see `core/bconf.c` parser for keys).

## Common pitfalls

- A new `#include` of a public header from a plugin needs `inc_include` in
  the meson target — check the relevant `plugins/<type>/<name>/meson.build`.
- The daemon hardcodes plugin discovery via `dlopen()` — adding a new
  plugin directory means adding a `subdir(...)` line in the relevant
  `plugins/<type>/meson.build`.
- After significant schema or KV changes, `scripts/freshstart.sh` to drop
  and rebuild the database. Pre-1.0 we do not write migrations.
