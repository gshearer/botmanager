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

The build is **warning-free, tree-wide** — a full clean `ninja -C build`
emits zero warnings, and that is the standard (`AGENTS.md`, the
`posixc-pro` skill). There are no grandfathered exceptions any more:
PLUGIN-AUDIT-1 lead (A) closed the last 77 on 2026-08-12. If you add one,
you own it. `-Wno-missing-field-initializers` is the single project-wide
suppression and its reason is stated in the root `meson.build`.

To check what you introduced, clean first — an incremental build only
reports on the files it recompiled:

```sh
ninja -C build -t clean && ninja -C build 2>&1 | grep -c 'warning:'
```

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
