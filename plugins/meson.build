subdir('db/postgresql')
subdir('protocol/irc')
subdir('method/command')

# extension/inference — the engine .so plus its command siblings. The
# engine defines inference_include (its own dir, where inference.h
# lives), so it must precede every consumer: the siblings below and
# method/chat.
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

subdir('method/chat')
subdir('feature/exchange')
subdir('service/coinbase')
subdir('service/kraken')
subdir('service/gemini')
subdir('feature/userquote')
subdir('feature/urlgrabber')
subdir('feature/whenmoon')
subdir('feature/whenmoon/strategy/example_sma_cross')
subdir('feature/whenmoon/strategy/testing')
subdir('feature/whenmoon/strategy/cp1')
subdir('feature/whenmoon/strategy/cp2')
subdir('feature/whenmoon/strategy/cc1')
subdir('feature/whenmoon/strategy/cc2')
subdir('feature/whenmoon/strategy/surf')
subdir('feature/whenmoon/strategy/mako')
subdir('feature/whenmoon/strategy/juggernaut')
subdir('feature/whenmoon/strategy/riptide')
subdir('feature/whenmoon/strategy/squall')
subdir('feature/whenmoon/strategy/keel')
subdir('service/openweather')
subdir('service/coinmarketcap')
subdir('service/searxng')
subdir('service/yahoofinance')
subdir('service/tmdb')
subdir('service/giphy')
subdir('service/rawg')
subdir('feature/crypto')
subdir('misc/math')
subdir('misc/eightball')
subdir('feature/stock')
subdir('feature/weather')
