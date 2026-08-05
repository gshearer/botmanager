# Character class sheets

Every file in this directory is one **character class** for the `attack`
duelling game — a flat-text sheet of the lines that class speaks when it
lands a blow, inflicts an affliction, heals, or kills. `attack reload`
scans the directory, and `characters/<type>.txt` becomes the class
`<type>`. **If a sheet does not parse, it does not exist**: the loader
rejects the whole file and logs the file, the line, and the reason.

This document is the authoring contract. It is written for an agent
adding a new class. The authoritative grammar lives in `attack_class.c`
(`atk_sanitize`, `atk_sec_meta`, `atk_class_parse`); where this file and
that code disagree, the code wins — say so in a commit and fix this doc.

> A longer, prose-style brief on *voice* and *craft* is in
> `../new-class-prompt.txt`. Read it once for tone. Read **this** for the
> rules the loader actually enforces.

---

## The one thing to understand first: the tier is not power

The engine rolls the damage number **first** — one uniform draw,
identical for every class — decides which tier that number fell in, and
*only then* asks your sheet for a sentence of that size. Your class never
touches the number.

A sheet with forty `critical` lines and a sheet with one hit exactly as
hard as each other. **More moves buys variety, never power.** The same is
true of afflictions (how *often* a turn becomes one is an engine roll,
identical for everyone) and heals (how *much* is restored is engine-owned).

So: **tag honestly, and never invent a mechanic.** A `minor` line
describing a decapitation is spoken over a 3-damage graze and reads as a
lie. There is no field for "resists fire", "strikes twice", "ignores
poison", cooldowns, weights, or probabilities — and there never will be.
Anything like that is flavour: put it in `desc` and let it be a fiction.

---

## File shape

Plain text, one class per file. Processing order matters:

- A line whose **first byte** is `#` is a comment.
- **Blank lines** are ignored.
- Everything **before the first `[section]`** is the `key: value` header.
- Inside a section, fields are separated by `|`. Each field is trimmed.
  A section splits on its first *N* pipes only; any further `|` is text.
- A move wrapped in one matched pair of surrounding quotes has them
  stripped automatically — but don't rely on it; just don't add quotes.

### Header — both keys required, before the first section

```
type: warrior      # must be EXACTLY the file stem, bare alphanumeric, <= 31 bytes
desc: Brawn and edged steel. Swords, axes, hammers, no mercy.   # <= 70 bytes, one line
```

`type` mismatching the filename, a non-alphanumeric `type`, a missing
key, or a `desc` over 70 bytes each rejects the file.

### The six sections

| section        | line shape           | required?                              |
|----------------|----------------------|----------------------------------------|
| `[damage]`     | `tier \| text`       | **yes**, and all four tiers present    |
| `[dot]`        | `kind \| noun \| text` | optional                             |
| `[heal]`       | `tier \| text`       | optional — but then **both** tiers     |
| `[death]`      | `text`               | optional                               |
| `[decay]`      | `kind \| text`       | optional                               |
| `[decay-kill]` | `kind \| text`       | optional                               |

- `[damage]` **tier** ∈ `minor medium major critical`. At least one line
  in **each** of the four tiers, or the file is rejected — the engine
  must be able to speak any roll it makes.
- `[heal]` **tier** ∈ `minor major` (only two bands). If you write a
  `[heal]` section at all, it needs **at least one `minor` and one
  `major`**; `medium`/`critical` are rejected here, not folded.
- `[dot]` lines carry **no tier** — the engine already rolled the number,
  and the line that inflicts an affliction names no number at all.
- `[decay]` is one tick of an affliction still working; `[decay-kill]` is
  the tick that finishes someone. Both are looked up by **kind**. Any
  kind you inflict but do not write a decay line for falls back to a
  plain engine line — you need not cover every kind.

### Affliction `kind` — one of exactly eight

```
bleed   poison   burn   rot   chill   curse   drain   shock
```

Deliberately generic, so a sword, a spell, and a plasma coil can all use
them. The kind chooses only a glyph and a colour on the scoreboard.

### Affliction `noun` — your class's word for the wound

`open wound`, `nanite bloom`, `binding word`, `swamp fever`. It is what
`{affliction}` expands to and it is stored with the affliction, so your
wording survives every tick it speaks. Rules, all enforced:

- **≤ 24 bytes**, lower case, **no leading article** (`a`/`an`/`the`),
  no control bytes.

---

## Macros — exact counts, no more and no less

Only these five tokens exist. Any other `{token}`, an unclosed `{`, or a
stray `}` rejects the move. Each token must appear **exactly** this many
times in a line's text:

| section        | `{attacker}` | `{target}` | `{damage}` | `{heal}` | `{affliction}` |
|----------------|:---:|:---:|:---:|:---:|:---:|
| `[damage]`     | 1 | 1 | 1 | 0 | 0 |
| `[dot]`        | 1 | 1 | 0 | 0 | 1 |
| `[heal]`       | 1 | 1 | 0 | 1 | 0 |
| `[death]`      | 1 | 1 | 0 | 0 | 0 |
| `[decay]`      | 1 | 1 | 1 | 0 | 1 |
| `[decay-kill]` | 1 | 1 | 0 | 0 | 1 |

Rationale for the zeros: a death line carries no number (the killing blow
already said it); an affliction line names the wound, a blow line must
not; a heal line names hit points restored, never damage dealt.

---

## Hard rules (rejections)

- **No `%` anywhere.** Not one. Sheet text is never a format string, and
  the loader refuses a `%` outright.
- **No control bytes** — this kills `\r`, `\n` (protocol injection) and
  `\x01` (the colour marker). No markdown, no emoji, no colour codes.
- **≤ 255 bytes per move** after trimming (the storage bound). Longer is
  rejected as "too long".
- No curly braces other than the five named macros above.
- `[damage]` present with all four tiers; `[heal]`, if used, both bands.

## Soft rules (warnings — the sheet still loads, but fix them)

- **20–100 moves total**, weighted heavily toward `[damage]`. Outside
  that range logs a warning.
- **≤ 128 moves per section** (a hard array bound): moves past it are
  silently **dropped** with a warning, not rejected.
- **One sentence per move, ≤ 150 characters** as a craft target (the hard
  bound is 255). Short enough to read at a glance in a chat window.
- **Every line different from every other**, in verb as well as noun.
- Match words to the tier: nothing in `minor` sounds fight-ending; only
  `critical` keeps the `CRITICAL damage!` register (see the examples).

---

## Registry is full — read before adding a file

There are currently **32 sheets and exactly 32 registry slots**
(`ATK_CLASSES_MAX`). Adding a 33rd class means **removing one first**, or
raising `ATK_CLASSES_MAX` in `attack.h` and rebuilding. A sheet beyond
the cap is a load failure, not a silent no-op.

### Existing classes — do not duplicate a niche

Pick a distinct weapon set *and* a distinct flavour of harm. The current
roster (`desc` line of each sheet):

| type | desc |
|---|---|
| alchemist | Vials, acids, and volatile tonics. Balanced brews to harm or cure. |
| astrologer | Retrograde planets and star charts. Predicts moves, aligns chakras. |
| barista | Caffeine injections and hot steam. Energy boosts and scalding splashes. |
| berserker | Pure bloodlust and fury. Relentless strikes that bleed the user. |
| bionic | Slow-motion running and synth SFX. Judo chops and styrofoam boulders. |
| bureaucrat | Red tape and stamped denials. Stalls the enemy with admin misery. |
| captain | Torn velour and dramatic pauses. Flying double-fists and monologues. |
| cleric | Borrowed authority. Smites, hammers and mends the fallen. |
| conspiracy | Red string and wild theories. Confuses with collateral mental damage. |
| dj | Dropping the bass, shifting the vibe. Sonic disruption, regenerative beat. |
| dotcommer | Unlimited VC, zero plan. A burn rate that destroys the market and self. |
| flamer | ALL CAPS USENET RANTS. Toxic rhetoric and lingering flame wars. |
| florist | Thorns and blossoms. Toxic pollen and soothing aromas. |
| hacker | Zero-days and payloads. Corrupts systems and scrambles target code. |
| influencer | Viral clout. Heals the team through parasocial validation. |
| lawyer | Cease and desist. Drains the will to live with billable hours. |
| mechanic | Heavy wrenches and WD-40. Dismantles armor, patches teammates. |
| monk | Inner balance and swift strikes. Fists, footwork, serene self-repair. |
| necromancer | Takes what is owed, sets the dead to collect the rest. |
| paladin | Shield and sacred oaths. Punishes the wicked, protects the faithful. |
| phreaker | Blue boxes and 2600Hz. Routes devastating long-distance tolls. |
| pylon | A humming crystal. Alters dimensional weather, restorative frequencies. |
| pyromancer | Living kindling. Unstable flames, scorching heat, high risk. |
| ranger | Wild instinct and long-range focus. Traps, tracking, unerring shots. |
| redshirt | Expendable security detail. Fires recklessly before being vaporized. |
| rogue | Quiet, quick and piercing. Daggers, bows, spears, poison. |
| shaman | Drums, rattles and a heavy staff. Slow harm, some mending. |
| sleestak | Hissing and slow in a rubber suit. Crossbows and lingering swamp fever. |
| sysop | God of the BBS. Drops connections, bans users, grants download quotas. |
| warrior | Brawn and edged steel. Swords, axes, hammers, no mercy. |
| webmaster | Blinking text and under-construction GIFs. Blinds foes, patches links. |
| wizard | Raw energy, delivered directly. Fire, force and frost. |

Anyone adding or removing a sheet updates this table in the **same
commit**. It is the only thing stopping the next class from being the
fourth sword user.

---

## Minimal valid skeleton

The smallest sheet that loads: header + `[damage]` with all four tiers.
Everything else is optional.

```
type: example
desc: A one-line pitch, seventy bytes or fewer.

[damage]
minor    | {attacker} scuffs {target}'s cheek in passing for {damage} damage.
medium   | {attacker} folds {target}'s ribs inward for {damage} damage.
major    | {attacker} drives {target} to the floor and does not stop — {damage} damage.
critical | {attacker} ends {target} where they stand. {damage} CRITICAL damage!
```

An optional affliction, heal, and finish, showing the wider shapes:

```
[dot]
bleed  | open wound | {attacker} leaves an {affliction} under {target}'s ribs.

[heal]
minor    | {attacker} closes a cut on {target} and restores {heal}.
major    | {attacker} knits {target} back together for {heal}.

[death]
{attacker} lowers {target} to the floor and steps over them.

[decay]
bleed  | The {affliction} {attacker} left has not closed; {target} loses {damage} damage.

[decay-kill]
bleed  | The {affliction} {attacker} opened runs {target} out of blood mid-step.
```

---

## Verifying

The gate is **`attack reload` reporting zero rejections** — not the
provenance of the text. Each rejection logs the file, the line number,
and the reason; fix that line and reload again.

Where possible, prove it before the daemon is involved: the loader
compiles standalone. Stub `clam`, `mem_*`, `util_rand`, `validate_alnum`,
and `atk_tunables_load`, `#include` the real `attack_class.c`, and run
the directory through `atk_class_load()` under sanitizers. `ACHIEVED.md`
("The sheets re-authored") records a worked example.
