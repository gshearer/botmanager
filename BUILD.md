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

The warning set is `-Wall -Wextra -Wpedantic` (`warning_level=3`) plus
`-Wformat-security`, which is in none of the other three. The default
`build` configuration is also **hardened**: `-fstack-protector-strong`
and `-fstack-clash-protection` unconditionally, and `-D_FORTIFY_SOURCE=3`
whenever optimization is on and no sanitizer is. FORTIFY turns on
`warn_unused_result` for `read`/`write`, so a discarded return there is a
build error's worth of noise — and a `(void)` cast does **not** silence
it. Core's eventfd wake/drain goes through `util_evfd_wake()` /
`util_evfd_drain()`; do not open-code one.

A sanitizer configuration deliberately drops FORTIFY — it redirects the
same libc entry points ASan replaces, and a `__chk` abort names far less
than an ASan report does:

```sh
meson setup build-asan -Db_sanitize=address,undefined -Doptimization=1 -Ddebug=true
```

To check what you introduced, clean first — an incremental build only
reports on the files it recompiled:

```sh
ninja -C build -t clean && ninja -C build 2>&1 | grep -c 'warning:'
```

`scripts/gen_version_h.sh` runs every build and bumps the `BUILDNUM` file
at the project root.

## Test

```sh
ninja -C build && cd build && meson test
```

Three suites, in `tests/`. They are **deliberately tiny and stay that
way**: the suite covers silent wrongness only — a secret written to a
log, a fetch of a private address, forged formatting, a person's fact
quietly overwritten. Loud, recoverable, plugin-local failures are
untested by design, so most changes owe no test; verify those by
building and exercising the running daemon via `botmanctl` and
`ircspyctl` as before. The reasoning is root `TODO.md` axes A27/A28/A31.

| Suite | Covers |
|---|---|
| `tests/test_util.c` | `util_redact_url`, `util_url_is_safe_https`, `util_b64_*` |
| `tests/test_colors.c` | `color_markup_translate` |
| `tests/test_fact_merge.c` | the FACT-1 fact merge ladder |

`test_fact_merge` needs Postgres, because the ladder *is* SQL: it
renders the same `MEM_FACT_OBSERVE_SQL` template `memory.c` sends
against a TEMP table that shadows `dossier_facts`, inside a rolled-back
transaction. It reads `botman.conf` itself and **skips** when the
database is unreachable, so a green `meson test` is not proof it ran —
`meson test -v` shows its check count.

Both sanitizer configurations build and run the suites too, and that is
worth doing for anything touching them:

```sh
ninja -C build-asan && ASAN_OPTIONS=detect_leaks=1 build-asan/tests/test_util
```

## Outputs

| Path | What |
|------|------|
| `build/core/botman` | Main daemon binary |
| `build/tools/botmanctl`, `build/tools/ircspy`, `build/tools/ircspyctl` | CLI tools — see `tools/AGENTS.md` |
| `build/plugins/<type>/<name>/lib<name>.so` | Runtime-loadable plugins |
| `build/tests/test_*` | Test binaries — run via `meson test`, or directly |
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
- After significant schema or KV changes, `scripts/bm-wipe.sh` to drop
  and rebuild the database. Pre-1.0 we do not write migrations.
