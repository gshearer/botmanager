#!/usr/bin/env python3
# A reverse proxy that refuses ONE leg of a wikimedia fan-out.
#
# Every verb here that fans out has a rule: an empty result is only
# WM_NOT_FOUND when EVERY leg answered.  Getting that wrong is invisible
# until one leg is refused mid-flight, which live traffic manages perhaps
# once a session -- so this makes it every time.  It forwards to the real
# endpoint and 429s one leg with a Retry-After, which is what the service
# reads.
#
#   LEG_MODE=search    refuses list=search        -> wm_resolve_merge
#   LEG_MODE=claims    refuses every other        -> wm_claims_choose
#                      wbgetclaims
#   LEG_MODE=reverse   refuses every other        -> wm_reverse_choose
#                      haswbstatement search
#
# Run it, point the service at it, drive a query whose OTHER legs come
# back empty, and put the knob back:
#
#   LEG_MODE=claims python3 tests/wikimedia/legproxy.py &
#   botmanctl -- set kv plugin.wikimedia.api_base http://127.0.0.1:8791
#   botmanctl -w 15000 -- wiki Q49740 height     # 3 candidates, no value
#   botmanctl -- set kv plugin.wikimedia.api_base \
#       https://www.wikidata.org/w/api.php
#
# A refused leg answers with a Retry-After, so it ARMS the real backoff
# for that long -- the next live query is refused until it lapses.  That
# is the service working, not the rig leaking.
import http.server, urllib.parse, urllib.request, sys, os, itertools

MODE = os.environ.get("LEG_MODE", "search")   # search | claims | reverse
SEQ  = itertools.count()

UP = "https://www.wikidata.org/w/api.php"

class H(http.server.BaseHTTPRequestHandler):
  protocol_version = "HTTP/1.1"

  def do_GET(self):
    q = urllib.parse.urlparse(self.path).query
    refuse = False
    if MODE == "search":
      refuse = "list=search" in q and "haswbstatement" not in q
    elif MODE == "claims":
      # Refuse every other wbgetclaims leg, so a multi-candidate property
      # word has some legs answer and some refused -- the exact shape the
      # old `reached |=` read as "this item records no such property".
      refuse = "action=wbgetclaims" in q and next(SEQ) % 2 == 0
    elif MODE == "reverse":
      refuse = "haswbstatement" in q and next(SEQ) % 2 == 0

    if refuse:
      body = b'{"error":{"code":"ratelimited"}}'
      self.send_response(429)
      self.send_header("Retry-After", "31")
      self.send_header("x-envoy-ratelimited", "true")
      self.send_header("Content-Type", "application/json")
      self.send_header("Content-Length", str(len(body)))
      self.end_headers()
      self.wfile.write(body)
      sys.stderr.write("REFUSED %s\n" % q[:70]); sys.stderr.flush()
      return

    req = urllib.request.Request(UP + "?" + q,
        headers={"User-Agent": "botmanager-legproxy/1 (agent test)"})
    with urllib.request.urlopen(req, timeout=20) as r:
      body = r.read()
    self.send_response(200)
    self.send_header("Content-Type", "application/json")
    self.send_header("Content-Length", str(len(body)))
    self.end_headers()
    self.wfile.write(body)
    sys.stderr.write("forwarded %s\n" % q[:60]); sys.stderr.flush()

  def log_message(self, *a): pass

http.server.ThreadingHTTPServer(("127.0.0.1", 8791), H).serve_forever()
