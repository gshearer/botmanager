subdir('db/postgresql')
subdir('protocol/irc')

# extension/inference — the engine .so plus its command siblings. The
# engine defines inference_include (its own dir, where inference.h
# lives), so it must precede every consumer: the siblings below and
# method/text.
subdir('extension/inference/engine')
subdir('extension/inference/ask')
# imagine_cmd is dormant: it registers the same 'imagine' command as
# imagine_zimage, so loading both aborts startup (duplicate command).
# Kept in-tree; re-enable this line (and remove imagine_zimage) once an
# OpenAI-compatible Linux/CUDA image backend lands. See project memory
# IMAGE-GEN-1 / imagine_zimage pivot (2026-07-22).
# subdir('extension/inference/imagine')
subdir('extension/inference/imagine_zimage')
subdir('extension/inference/claude')
subdir('extension/inference/search')

subdir('method/text')
subdir('feature/exchange')
subdir('service/coinbase')
subdir('service/kraken')
subdir('service/gemini')
subdir('feature/userquote')
subdir('feature/attack')
subdir('feature/urlgrabber')
subdir('feature/note')
subdir('extension/whenmoon')
subdir('extension/whenmoon/strategy/example_sma_cross')
subdir('extension/whenmoon/strategy/testing')
subdir('extension/whenmoon/strategy/cp1')
subdir('extension/whenmoon/strategy/cp2')
subdir('extension/whenmoon/strategy/cc1')
subdir('extension/whenmoon/strategy/cc2')
subdir('extension/whenmoon/strategy/surf')
subdir('extension/whenmoon/strategy/mako')
subdir('extension/whenmoon/strategy/juggernaut')
subdir('extension/whenmoon/strategy/riptide')
subdir('extension/whenmoon/strategy/squall')
subdir('extension/whenmoon/strategy/keel')
subdir('extension/whenmoon/strategy/capstan')
subdir('extension/whenmoon/strategy/fathom')
subdir('extension/whenmoon/strategy/sextant')
subdir('extension/whenmoon/strategy/kedge')
subdir('service/openweather')
subdir('service/coinmarketcap')
subdir('service/searxng')
subdir('service/yahoofinance')
subdir('service/tmdb')
subdir('service/giphy')
subdir('service/rawg')
subdir('service/wordnik')
# reachyapi defines reachyapi_include, so it must precede every consumer
# (feature/reachycmd below, and the reachy protocol driver later).
subdir('service/reachyapi')
subdir('feature/reachycmd')
subdir('feature/crypto')
subdir('misc/math')
subdir('misc/eightball')
subdir('misc/roulette')
subdir('feature/stock')
subdir('feature/weather')
