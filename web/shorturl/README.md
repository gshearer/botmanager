# shorturl — the URL shortener

Every short link botmanager prints is a row in one table and two processes that
never meet:

| Half | Where it runs | What it does | Privilege on `urls` |
|---|---|---|---|
| `plugins/service/shorturl/` | the botman host, in the daemon | **mints**: generates a token, inserts the row, returns `<base_url>?<token>` | owner |
| `web/shorturl/` (here) | the **web server**, under nginx | **resolves**: looks the token up, counts the hit, sends a 302 | `SELECT`, `UPDATE` |

The table is the only thing between them, and `src/token.c`, `src/url.c` and
`src/config.h` — compiled into *both* — are why the two agree on what a token
and a target are. Change `SHORTURL_TOKEN_LEN` and the `CHECK` constraint the
plugin generates moves with it. `tests/test_shorturl_token.c` is the gate on
that pair; it is the one test that covers two binaries at once.

⚠ **The daemon cannot mint.** Its database role is granted `SELECT` and
`UPDATE` on `urls` alone. This is the internet-facing half, so an `INSERT`
privilege here would let anyone who took it point a link anywhere they liked.
The admin CLI's `add` therefore needs the owner's credentials, not the
daemon's.

## Build

`ninja -C build` produces both binaries into `build/web/shorturl/`, guarded on
libfcgi and libpq being installed (`pacman -S fcgi postgresql-libs`); without
them the rest of the tree still builds and this subdir simply declares nothing.

| Binary | Purpose |
|---|---|
| `shorturl.fcgi` | the redirect daemon — this is what you deploy |
| `shorturl` | admin CLI: `add <url>`, `list`, `delete <token>`, `version` |

`shorturl version` prints the botmanager build the binary came from, which is
the only way to tell what is actually running on the web host after a hand
copy.

## Deploy

1. Copy `build/web/shorturl/shorturl.fcgi` to `/usr/local/bin/` on the web
   server.
2. Write `/etc/shorturl/shorturl.env` with `SHORTURL_DB_HOST`, `_PORT`,
   `_NAME` (botmanager's database), `_USER`, `_PASS`, and optionally
   `SHORTURL_BASE_URL` and `SHORTURL_NOT_FOUND_URL`.
3. Install the unit pair and enable the socket — the commands are in the
   header of `shorturl.socket`.
4. nginx needs one `location` block; it is in the header of `shorturl.service`.

## Behaviour worth knowing

- **Redirects are 302, not 301**, with `Cache-Control: no-store`. A permanent
  redirect would be cached by the browser, repeat visits would never reach the
  daemon, and the hit counter would quietly stop counting. That is the one line
  to change if you ever want SEO link equity more than accurate counts.
- **Lookup and increment are one `UPDATE ... RETURNING`** — one round trip, and
  no window for a lost update under concurrency.
- **A malformed token costs a length check and eight range tests**, before any
  database round trip. On a public host that is the majority of the traffic.
- **A dropped connection is repaired on the next request**: libpq only
  discovers a dead connection when a command is attempted, so the retry is what
  keeps that from costing a visitor a 500.
- **One process, ~8 concurrent connections.** Measured on the live deployment:
  ~862 req/s and ~99% served at 8, ~58% at 20, median 6–7 ms end to end.
  Overflow is refused at the socket and surfaces as nginx 502 without reaching
  the database. Past a sustained 8, add processes — each holds one PostgreSQL
  connection, so size the count against `max_connections`.
