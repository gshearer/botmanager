# BRINGUP — from a fresh Reachy Mini SD card to a talking robot

Everything needed to rebuild the reachy voice stack from bare hardware. Read
this when the SD card dies, the robot is replaced, the 4090 host is rebuilt, or
someone asks "what would it take to do this again?"

**This is a runbook, not a design document.** The *why* behind every choice
lives in `TODO.md §C` (charter) and `ACHIEVED.md` (what shipped, with SHAs).
This file only tells you what to type and in what order.

## ⚠ Honesty about what is verified

We never rebuilt this robot from scratch — it arrived working on 2026-08-03 and
was brought up incrementally. So:

- **Phases 2–6 are MEASURED.** Every command was run against real hardware and
  is recorded in `ACHIEVED.md` with a commit SHA.
- **Phase 1 (the fresh image) is RECONSTRUCTED** from what we observed of the
  as-shipped system. It is our best account, not a tested procedure. Treat the
  step marked ⚠ UNVERIFIED with suspicion and correct this file the first time
  anyone actually does it.

## What is in git and what is not

Rebuilding is mostly about the things git does not carry:

| Piece | In git? | How it comes back |
|---|---|---|
| `senses/earbridge/*.c/.h`, Makefile, service, deploy.sh | ✅ | `git clone` |
| `senses/kokorod/*.c/.h`, meson, service | ✅ | `git clone` |
| `senses/whisper/` unit + `fetch-model.sh` | ✅ | `git clone` |
| `plugins/{service/reachyapi,feature/reachycmd,protocol/reachy}/` | ✅ | `git clone` + `ninja` |
| `scripts/reachy_mic_probe.sh` | ❌ *(`scripts/` is gitignored)* | recreate — see `scripts/AGENTS.md` |
| Model weights (whisper `.bin`, kokoro dir) | ❌ gitignored, GBs | Phase 3 |
| systemd units **installed** (host user units, robot system unit) | ❌ | Phases 2–3 |
| LLM registry rows (`senses-stt`, `whisper1`, …) | ❌ *(in Postgres)* | Phase 4 |
| KV values, bot identity, personality | ❌ *(in Postgres)* | Phases 4, 6 |
| SSH key on the robot | ❌ | Phase 1 |

---

# Phase 1 — the robot, from a fresh image

### 1.1 Flash and first boot ⚠ UNVERIFIED

Use Pollen's official Reachy Mini image for the **Wireless** variant (this unit
is Wireless/CM4, *not* the USB Lite — the daemon runs on the robot itself).
Ours reported reference image **2026-01-14**, Debian 13 (trixie), stock
Raspberry Pi OS built with pi-gen.

Join it to the network. Ours lives on a separate VLAN at
`reachy.iot.hiigara.shearer.tech` → `10.68.251.13`.

⚠ **The link is flaky.** SSH timed out on 2 of 6 attempts during recon and two
HTTP calls returned empty mid-run. Ping 4–59 ms, mdev 27 ms. Everything you
build against this robot needs retry/backoff from the start — that is not
pessimism, it is measured.

### 1.2 SSH access

Default login is user `pollen`, password `root`. Install a passphrase-less key
immediately; every script in this project assumes key auth:

```sh
ssh-copy-id pollen@reachy.iot.hiigara.shearer.tech
ssh pollen@reachy.iot.hiigara.shearer.tech true   # must not prompt
```

### 1.3 ⚠ THE MICROPHONE RIBBON TRAP — check this before believing anything

**The mic ribbon can be inserted upside-down in its ZIF slot, and the cable's
natural bend actively fights the correct orientation.** Ours shipped that way.
It cost most of a session and produced a *drafted hardware-RMA verdict* that
was completely wrong.

Symptoms: raw capture returns literal digital silence (`peak=0 rms=0.00
nonzero=0/96000`), and the XMOS DSP reads zero energy on all four beams while
otherwise responding normally to register reads.

**Reseating it is not enough — it must be FLIPPED.** Verify before proceeding:

```sh
curl -s http://reachy.iot.hiigara.shearer.tech:8000/api/state/doa
# {"angle":<rad>,"speech_detected":<bool>} with angle that MOVES as you speak
# from different directions. A permanently 0.0 angle means the ribbon.
```

Red herrings that are **not** faults: `usb 1-1.1: clock source 1 is not valid`
and `cannot get freq: err -71` in `dmesg` (they appear only when a raw ALSA
stream is opened), and `imx708 10-001a: failed to read chip id` (that is the
CM4's second, unpopulated CSI port).

### 1.4 The permanent DO-NOT list

- **Never `apt upgrade` / `dist-upgrade`.** Nothing here needs it; the daemon
  depends on a Rust GStreamer signalling plugin and specific camera overlays.
- **Never flash the XVF3800 firmware.** 2.1.2 is correct and current for
  Wireless; 2.1.3 is beta-units-only per the vendor's own changelog.
- **Never `pip` into the daemon venv.** Reachy software is a PyPI package
  updated through the daemon's own `POST /update/start`, never pip or apt.
- **Never open `hw:0,0` for capture** — it is exclusive and steals the dsnoop
  slave, killing the daemon's own capture, face tracking and DoA. Use
  `reachymini_audio_src`.
- **Never call `/api/media/release`** for audio. It is unnecessary: capture is
  shared (see 1.5).
- **Never USB re-enumerate** (`/sys/bus/usb/.../authorized` 0→1). Tried once;
  broke raw ALSA access until a power cycle and proved nothing.

### 1.5 What the stock image already gives you

Confirm rather than create — **`~/.asoundrc` ships with the image** (ours is
dated Jan 15 2026, untouched by us):

```sh
ssh pollen@reachy… 'head -20 ~/.asoundrc'
# reachymini_audio_src = dsnoop  (SHARED capture, 16 kHz 2ch on hw:0,0)
# reachymini_audio_sink = dmix   (SHARED playback, bound to exactly 2 channels)
```

Both matter enormously and neither is ours:

- **dsnoop is why earbridge can record while the daemon holds the mic.**
  Measured: 159,538/160,000 non-zero samples captured while
  `media/status` stayed `released:false` and DoA kept serving.
- ⚠⚠ **dmix is bound to 2 channels, so MONO PLAYBACK IS SILENTLY
  UNPLAYABLE.** A mono WAV uploads fine, `play_sound` returns
  `200 {"status":"ok"}`, the daemon logs a normal playback line, **and nothing
  comes out — no error on any surface.** This cost most of a session. All
  uploaded audio must be **stereo**; sample rate is a non-issue (the daemon
  resamples, its own assets are 44.1 kHz).

Build prerequisites are also already present on stock Debian 13 — `gcc`, `make`
and ALSA headers. **No `apt install` is needed.** Verify:

```sh
ssh pollen@reachy… 'which gcc make && ls /usr/include/alsa/asoundlib.h'
```

### 1.6 Verify all four senses before building anything

```sh
H=http://reachy.iot.hiigara.shearer.tech:8000
curl -s -X POST $H/api/motors/set_mode/enabled     # torque on
curl -s -X POST $H/api/move/play/wake_up           # head rises ~45 mm
curl -s -X POST $H/api/volume/set -H 'Content-Type: application/json' -d '{"volume":60}'
curl -s -X POST $H/api/volume/test-sound           # speaker — listen for it
curl -s -X POST $H/api/media/tracking/enable -H 'Content-Type: application/json' -d '{"weight":0.6}'
curl -s $H/api/media/tracking/face                 # detected:true when you are in frame
curl -s $H/api/state/doa                           # angle moves as you speak
```

⚠ **Motors boot `disabled`, and the daemon is asymmetric about torque:**
`goto_sleep` disables the motors itself at the end of its move, but `wake_up`
never enables them. A limp robot accepts every move command, returns a normal
`{"uuid":…}`, completes, and does not move — with no error anywhere. That is
why the `set_mode/enabled` line above comes first and is not optional.

If the robot "ignores" you, check `GET /api/motors/status` first — or ask the
bot, since `show bot <name> robot` reports the motor mode in words.
`/bot <name> wake` does both steps for you (shipped `4ef0cef`); this section is
the by-hand path for a robot with no botman in front of it.

---

# Phase 2 — earbridge, the ear (runs ON the robot)

earbridge is the only artifact in this project that executes on another
machine: raw mic PCM never leaves the robot over REST, so segmentation has to
be local. It is built **on the CM4** by a plain Makefile.

```sh
senses/earbridge/deploy.sh          # copies sources, builds remotely (aarch64)
```

Then the privileged step, deliberately left to a human because installing a
system unit on a device you cannot easily reimage is not a convenience:

```sh
ssh pollen@reachy.iot.hiigara.shearer.tech
sudo cp ~/earbridge/earbridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now earbridge
systemctl status earbridge
```

Verify:

```sh
curl -s http://reachy.iot.hiigara.shearer.tech:8090/health
# {"ok":true,...,"floor_db":-35.0}
```

### ⚠ Tuning `--floor-db` — expect to do this, it is not a constant

The XVF3800's hardware VAD fires on the **Raspberry Pi's own CPU fan**, and
whisper transcribes fan noise into confident English ("Thank you." being the
favourite). earbridge therefore gates each utterance on its loudest 250 ms
window and drops anything below `--floor-db` (default **−35 dBFS**, set
explicitly in the unit file).

**The fan is load-dependent — it speeds up when the robot is busy**, so the
noise floor rises exactly while transcribing, synthesizing or moving. A floor
tuned in a silent room can be too low mid-conversation.

```sh
curl -s http://reachy…:8090/health   # watch quiet_discards vs utterances
```

- real speech being dropped → **lower** it (more negative), e.g. `-40`
- fan still getting through → **raise** it, e.g. `-32`
- `-90` or below disables the gate entirely

Re-measure with **`scripts/reachy_mic_probe.sh`**, which records raw
VAD-ungated audio and prints a quiet-floor / loudest-window / separation
summary. Do **not** reach for `--min-ms`: artefact audio runs 1–8 seconds, only
the transcripts are short.

**Mic gain is a dead end** — `amixer -c 0 numid=10` is a 0 → −60 dB attenuator
already at unity. There is nothing to turn down, and attenuating drags real
speech toward the level whisper hallucinates on.

---

# Phase 3 — the speech hosts (run on the 4090 host)

Both are **user** systemd units, not system units, and neither is enabled by
default — boot persistence is a deliberate operator choice.

### 3.1 whisper — STT on :8004

Distro package, not a source build ([[feedback-prefer-distro-packages]]):

```sh
# AUR: whisper.cpp-cuda (was 1.9.1). Verify CUDA linkage is real:
ldd /usr/bin/whisper-server | grep -E 'ggml-cuda|cudart|cublas'

senses/whisper/fetch-model.sh          # ggml-large-v3-turbo, ~1.6 GB, verifies sha256

install -Dm644 senses/whisper/whisper-server.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now whisper-server
```

The unit runs with `--suppress-nst -nth 0.8`, the second layer against
hallucination. It is genuinely *second* — with both flags live, a silent-room
recording still transcribed as "Amen." Gate at the ear, not here.

### 3.2 kokorod — TTS on :8003 (ours, in C)

```sh
# AUR: sherpa-onnx (was 1.13.4). Ships a working pkg-config, so meson finds it.
```

⚠ **Fetch `kokoro-multi-lang-v1_0`, NOT `v1_1`.** v1.1 is the **Mandarin**
model — its entire English voice set is `af_maple af_sol bf_vale`, and it has
none of `af_heart` / `af_bella` / `am_michael`. The higher version number is a
trap. Get it from the sherpa-onnx TTS model releases and unpack to
`senses/kokorod/models/kokoro-multi-lang-v1_0/` (must contain `model.onnx`,
`voices.bin`, `tokens.txt`, `espeak-ng-data/`, `lexicon-us-en.txt`).

```sh
ninja -C build                          # kokorod is meson-guarded on sherpa-onnx
install -Dm644 senses/kokorod/kokorod.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now kokorod
curl -s http://127.0.0.1:8003/health    # {"ok":true,"voices":53}
```

⚠ **53 voices, not 54.** The upstream Hugging Face list has 54; this model
omits `em_santa`. A positional voice table built from the HF list is off by one
after `em_alex` and speaks in the wrong voice with nothing logged. The
authoritative mapping is in the model's own ONNX metadata:
`strings -n 6 model.onnx | grep -A1 speaker_names`. `af_heart` = sid 3.

kokorod peak-normalises to −3 dBFS and emits **stereo** — both mandatory, not
cosmetic. Raw Kokoro output peaks at −12 dBFS and is inaudible over the wobble
motors; mono is silently unplayable (1.5).

### 3.3 Gotcha: `systemctl --user` from an agent shell

Fails with "Failed to connect to user scope bus" until:

```sh
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus
```

---

# Phase 4 — register the speech backends with botman

These rows live in Postgres and survive restarts, so this is once-per-database,
not once-per-boot.

```sh
build/tools/botmanctl "llm add service senses-stt http://127.0.0.1:8004"
build/tools/botmanctl "llm add service senses-tts http://127.0.0.1:8003/v1"
build/tools/botmanctl "llm add model stt whisper1 senses-stt whisper-large-v3-turbo"
build/tools/botmanctl "llm add model tts kokoro1  senses-tts kokoro"
build/tools/botmanctl "show llm models"        # expect whisper1 (stt), kokoro1 (tts)
```

⚠ **`senses-stt` has NO `/v1`; `senses-tts` does.** whisper-server's route is
`/inference` — `/v1/audio/transcriptions` 404s.

⚠ **Never run `llm service <n> refresh` against either.** Neither host serves
`/models`, so the permanent "model_id not in this service's cached /models
list" warning on both rows is correct and expected.

Point the robot plugins at the hardware:

```sh
build/tools/botmanctl set kv plugin.reachyapi.base_url   http://reachy.iot.hiigara.shearer.tech:8000
build/tools/botmanctl set kv plugin.reachyapi.bridge_url http://reachy.iot.hiigara.shearer.tech:8090
```

---

# Phase 5 — build and load the plugins

Three plugins, layered protocol/misc → service (`TODO.md §C4`):

```sh
ninja -C build
build/tools/botmanctl "plugin load reachyapi"    # PLUGIN_SERVICE, no commands
build/tools/botmanctl "plugin load reachycmd"    # the bot-scoped robot verbs
build/tools/botmanctl "plugin load reachy"       # the method driver (ears)
build/tools/botmanctl "plugin audit reachyapi"
```

⚠ Load order matters now: `reachycmd` requires **both** `service_reachyapi`
and `method_reachy`, because its verbs write the instance KV the driver owns.

⚠ Every robot verb lives under `/bot <name>` or `/show bot <name>` and is
therefore **admin-gated by its parent** — `cmd_invoke()` checks nothing, so the
child's own registration is decorative and matches the parent deliberately
(operator decision, 2026-08-05, superseding RCH-1-PERMS for this surface). An
`ircspy` nick is anonymous and gets "Permission denied", so verify with a
registered identity:

```sh
build/tools/botmanctl -u doc "bot mini say hello there"
```

⚠ **Async replies never come back through botmanctl** (`bctl_reply_target` is
cleared when dispatch returns). Judge by the *action*: check
`/tmp/reachy_mini_sounds/reachy_say.wav` on the robot is seconds old — at
24 kHz stereo 16-bit, `bytes / 96000` ≈ seconds of speech.

---

# Phase 6 — give a bot ears

```sh
build/tools/botmanctl "bot add <name> chat"
build/tools/botmanctl "bot addmethod <name> reachy"
build/tools/botmanctl set kv bot.<name>.reachy.speaker <operator-handle>
build/tools/botmanctl "bot start <name>"
build/tools/botmanctl "show tasks"      # expect a reachy_ears_<name> persist slot
```

Two gates silently swallow everything during bring-up:

- `bot.<name>.behavior.chat.enabled` needs **`1`, not `true`** — it is a BOOL
  that rejects the word.
- A bot with no `behavior.personality` reaches `interject path=reply` and stops
  with "no active personality set". Personality path is **absolute** (the
  daemon's CWD is the repo root).

Attention policy is per-bot KV `bot.<name>.reachy.attention.*` (`open|name`,
`names`, `strict`, `window_s`). `name` is the privacy mode, and since §A1 it
means it: only a line carrying the bot's name is an address, and everything
else the driver admits — the whole room in `open` mode, the tail of a
conversation in `name` mode — is delivered **ambient**, which the chat plugin
witnesses but never answers as conversation.

`attention.names` entries may be phrases; punctuation between their words is
ignored, so `hey mini` matches whisper's "Hey, Mini, ...". A phrase only
*adds* a trigger unless `attention.strict = 1`, which is what stops the bare
bot name from waking it on its own.

⚠ `bot.<name>.reachy.barge_in` **defaults to 0 and should stay there** until
the robot's cooling fan is quieter. The microphone array's `speech_detected`
flag reads true in ~49% of a *silent* room, so barge-in interrupts the bot
rather than for it — `ACHIEVED.md §BARGE-TRUTH` has the measurement and the
re-test.

---

# Verification — the whole chain, end to end

```sh
curl -s http://reachy…:8090/health          # ear alive, floor_db set
curl -s http://127.0.0.1:8004/inference -F file=@<a wav> -F response_format=json
curl -s http://127.0.0.1:8003/health        # {"ok":true,"voices":53}
build/tools/botmanctl "show bot mini robot"  # live DoA bearing
build/tools/botmanctl -u doc "bot mini say the ear hears and the mouth answers"
build/tools/botmanctl "say <bot> voice the mouth is wired to the brain"
```

That last line is the **mouth-only rig**: it drives `method_send` without
needing the ear, the brain or a human. ⚠ It reports **`send failed` even when
the line is spoken perfectly** — that is the `SUCCESS == false` trap in the
`say` command, not a robot fault. The truth is `/tmp/botman.log`: look for
`reachy … says "…" (… tts N ms, upload N ms, play N ms)`.

Then speak to the robot and confirm the transcript reaches the brain in the
CLAM stream (`botmanctl -S 6 -r 'reachy|llm'`) and that the answer comes back
out of the speaker with the head wobbling.

---

# Keeping this file honest

**Update it in the same commit as the change it describes.** A bring-up
document that has drifted is worse than none — it will be trusted at exactly
the moment nothing else works.

Specifically, touch this file whenever you: add or move a systemd unit, change
a model or its path, add a KV the bring-up needs, change a default that
bring-up depends on (`--floor-db` especially), or discover a new hardware trap.

Related: `TODO.md §C` (charter, every measured fact) · `ACHIEVED.md` (what
shipped, with SHAs, and the per-chunk gotchas) · `scripts/AGENTS.md`
(`reachy_mic_probe.sh`) · `TASKROUTER.md` (the reachy row).
