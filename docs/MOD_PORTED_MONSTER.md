# Porting a monster and scripting its AI — the Lua surface

**Status (2026-08-29):** the runtime is `framework/prx/mods/lua_host/scripts/mhfu_port.lua`;
the worked example is `brute_showcase.lua`. Both are plain memstick mods — no PRX rebuild, no
recompile, hot-reloadable.

This document is for someone writing a mod. For *why* the design is shaped this way, read
`AI_SCRIPTING_ENGINE.md` §33–34; for how a port is produced from an MHP3rd file, read
`BRUTE_TIGREX_PORT.md` and the `monster-porting` skill.

---

## 1. The one fact the whole API is built on

A big monster runs on **two independent channels**:

| channel | driven by | decides |
|---|---|---|
| **animation** | executor `0x09AC5228(entity, a1)` | which **clip** the body-part slots play |
| **behaviour** | `act_set` → `entity+0x298` / `+0x299` | which per-action MIPS code runs — **hitbox, effects, damage** |

Forcing `a1` cannot change what an attack *does*. Writing `(main, sub)` runs a complete,
damaging move — verified live: a Lua-written pair killed the hunter.

A port inherits a specific defect from this. The host's behaviour handler asks for host
animation id *N*, and the clip sitting in the port's slot *N* is whatever the packer put
there — filed **by position, not by meaning**. That is why the ported Brute threw a rock while
the engine was correctly making him roar.

**The fix is a declared mapping**, and that is all `mhfu_port` is:

```lua
moves = { charge = { main = 3, sub = 6, clip = "charge" } }
```

`port:play("charge")` writes the behaviour pair *and* latches the clip, so the executor
dispatch that follows is overridden to the port's own charge animation. The host's opinion
about which clip belongs to that move is discarded.

This is why it generalises to a source monster with **no host analogue** (a Zinogre). You are
not aligning the port to the host. You pick any host behaviour whose *physics* you want and
put your own clip on top of it.

---

## 2. A minimal mod

```lua
-- Load order in the mods directory is readdir order — not alphabetical, not
-- guaranteed. These two lines are the contract; copy them verbatim.
mhfu.port = mhfu.port or { _queue = {} }
mhfu.port.mod = mhfu.port.mod or function(n, f) mhfu.port._queue[n] = f end

mhfu.port.mod("my_mod", function(P)
  local mon = P.define{
    name    = "zinogre",
    species = mhfu.MON_TIGREX,          -- the host species the port rides on
    replace = { mhfu.MON_GIADROME },    -- quest monsters to swap for it
    pac     = "zinogre_v2.bin",
    orig    = "file_06185.bin.orig",
    fid     = 6186,

    clips = { charge = 61, roar = 50 }, -- YOUR clip vocabulary, by executor a1
    moves = {                           -- the alignment
      charge = { main = 3, sub = 6, clip = "charge" },
      roar   = { main = 0, sub = 2, clip = "roar"   },
    },
  }

  mon:brain(function(s)
    if s.same_section and s.engaged and s.dist > 1500 then
      mon:face(s.px, s.pz)
      mon:play("charge")
    end
  end)
end)
```

Drop it in `ms0:/PSP/PLUGINS/mhfu_framework/mods/` next to `mhfu_port.lua` and cold-boot.

---

## 3. API

### `P.define(spec) -> port`

| field | meaning |
|---|---|
| `name` | identifier, also the hot-reload key |
| `species` | host species the port replaces (`mhfu.MON_TIGREX`) |
| `pac` / `orig` / `fid` | relocate-inject arguments; `pac` replaces `orig` in RAM, nothing is written to disk |
| `inject_dir` | defaults to `ms0:/PSP/PLUGINS/mhfu_framework/inject` |
| `replace` | list of quest monster ids to swap for `species` at `QUEST_TARGETS_BUILDING` |
| `clips` | name → executor `a1` |
| `moves` | name → `{ main, sub, clip }` (or `anim = <a1>` to skip the vocabulary). `latch = <n>` overrides how many executor dispatches the clip covers — **the default is 1**, because one forced pair runs a SEQUENCE of sub-actions (a seven-tick `(2,1)` asked for a1 15, 11, 19 and 18 in turn) and overriding all of them restarts the clip from frame 0 each time |

The injector is armed **once per boot** however many times `define` runs, so hot-reloading a
mod file is safe.

**⚠️ The declarative half of that spec now has a home outside Lua.** `ports/<name>.toml`
(`mhfu_monster_editor.manifest`) carries `name` / `species` / `pac` / `fid` / `replace` / `clips`
/ `moves` — plus the flag soup the PAC was built with, which used to survive only in a commit
message. The runtime does **not** read it yet, so a mod still spells the tables out; but the
manifest is what the porter and the validator agree on, and it is where a wrong clip id gets
caught. `python -m mhfu_monster_editor.validate ports/<name>.toml --pac <built>` rejects a slot
that is not in the build and warns on one that holds filler — both of which look identical to
"the latch didn't work" in game. Keep the two in step; when they disagree, the manifest is the
one that was checked.

### `P.hit(port_name, tbl)` — the port's own hurtboxes and damage grid (2026-09-11)

Not hand-written: `python -m mhfu_monster_editor.runtime ports/<name>.toml` (or the editor's
Parts panel, *export runtime table*) generates `scripts/<name>_hit.lua` from the manifest's
`[[hurtbox]]` and `[[hitzone]]` blocks, and that module makes the call. Keyed by PORT name so
it and the brain module load in either order.

| field | |
|---|---|
| `species` | the host species id whose tables are overwritten |
| `id` | eight hex digits over the content — `framework.log` prints it on `HIT TABLES APPLIED` |
| `volumes` | list of `{ bone, shape, hitzone_row, part, flags, radius, ax, ay, az, bx, by, bz }` in the `0x28` record order, or `nil` to leave the host's set alone |
| `grid` | list of states, each 7 rows of 10 percentage bytes, or `nil` |
| `attack_tables` | `{ volumes, records, n_sets, n_records }` — the host overlay's set-pointer table and `0x18` record array VAs (em75: `0x09D60768` / `0x09D60848`), out of `species/emNN.json`. Required with either of the next two (#33) |
| `attack_sets` | `{ [set] = { cap, volumes = { <same 12-literal rows, row/part 0> } } }` — each set written IN PLACE over the host's set of that index, then a sentinel. `cap` is the host's record count: the in-place limit AND the fingerprint — a set whose live count differs is REFUSED, nothing written |
| `attacks` | list of `{ id, power, element, volume }`, `nil` = the host's byte stands; written to `records + id*0x18` at `+0x02` / `+0x09` / `+0x0A` |

**Where it lands, and why in place.** Both tables live in `game_task.ovl`'s species row
(`0x09BB87C0 + species*0x1D0`): `+0x240` points at the collision-record set this species
walks (em75 holds four; species 75's is `0x09D58CD0`, 42 records) and `+0x2FC` at the
hitzone state table. The volumes are written over the original records, then a
`bone = 0xFFFF` sentinel — both engine walkers stop there — so a shorter list is fine and a
longer one is truncated at the original count (measured on first contact, logged as
`host set ... holds N record(s)`). Nothing is relocated, no pointer is rewritten, no PRX
change. Applied once the entity is live in-area (`screen_state == 17`), re-checked every
tick against one record and one grid byte, and re-applied with a log line if the live
bytes changed under it. Each hit the port takes is logged as `HIT #n -amount hp=…`.

⚠️ Species data is map-wide: with the port REPLACING the host it is the only em75 in the
quest, so the table is his alone. Beside a native Tigrex (the ADD path) it would re-skin
the native's hurtboxes too — and re-arm its attacks. Status: the grid half was proven live
2026-06-28; the hurtbox volumes half on 2026-09-11 (#19, 50+ hits through one injected
sphere); the attack sets' in-place replacement by RAM poke on a native Tigrex (#37,
645 -> 152 -> 1381 units) and the generated `attack_sets` / `attacks` path live the same
day (hot-reloaded into a running quest: `1 attack set(s)/10 volume(s)` applied, the ported
Zinogre's lunge connected through the authored sphere, the power lever took).
`mhfu_monster_editor/tests/lua_attack_harness.lua` runs it offline over the real em75
bytes, refusal included. ⚠️ `deploy` copies `mhfu_port.lua` with the module: a stale
library on the memstick silently drops the fields it does not know.

### `port:brain(fn)`

`fn(s)` runs on every tick with the monster alive. `s` carries:

| field | |
|---|---|
| `ent` | entity pointer |
| `tick` | tick counter |
| `x, y, z` / `px, py, pz` | monster / player world position |
| `dist` | XZ distance between them |
| `travelled` | units the MONSTER moved since the last tick — the signal that says which behaviour pair is running |
| `closing` | units the GAP shrank since the last tick. Not the same number: a hunter walking into a charge contributes his own speed. **Project reach-the-player decisions off this**, or off `max(travelled, closing)` |
| `section`, `area`, `same_section` | |
| `reframed` | true on the first tick after a section change: `dist` is readable, `travelled`/`closing` are not |
| `engaged` | `entity+0x5DC` — **detected/pursuing, NOT full combat.** See the warning below |
| `target` / `targets_player` | `entity+0x2F4`, the resolved combat target pointer, and whether it is the hunter. This is the "is he after **me**" read `engaged` is not |
| `acquired` | `entity+0x2A4`, the aggro-eval's target-acquired flag |
| `main`, `sub` | the live behaviour pair |
| `move` | the scripted move still running, or `nil` when it has ended |
| `last_move`, `last_move_ticks` | what the previous scripted move was and **how many ticks it survived**. A forced pair the host handler declines ends on its first tick — this is the only way to find that out, and what a candidate shortlist rotates on |
| `pinned`, `slip` | is the coordinate lock on, and how many units it had to correct last tick |
| `hp`, `player_hp` | |

### `port:play(name [, min_gap])`

Writes the behaviour pair and latches the clip. Returns `false` if it declined.

Refuses to re-issue the **same** move within `min_gap` ticks (default 2). That guard is not
politeness: re-entering the executor every tick restarts the move before it ever reaches its
hitbox frames — the measured failure mode of a held `a1` force — and repeated forcing makes the
engine OR in the exhaustion bits and halt the AI outright.

### `port:latch(a1 [, uses])`

Puts a clip on the ANIMATION channel and writes **no** behaviour pair. This is
the instrument under `play()`, not a way to script: it exists to answer "which
clip is in slot N of this build, and does it run?" without the behaviour
handler's opinion in the measurement.

It lands on the next **executor dispatch**, not immediately — and how long that
takes is a property of what the monster is doing, not a constant. Parked 2200
units away the ported Zinogre sat in one looping idle for 65 s at a stretch and
200 s of probing produced **five** dispatches; in a fight they come every second
or two. **So advance a probe on consumption, never on a timer** (`tools/…` and
`zinogre_clips.lua` do exactly this).

⚠️ And see §4b: a latched clip is overwritten by the engine's next dispatch. It
proves the clip exists and plays; it cannot hold it.

### `port:release()` · `port:face(x, z)` · `port:pin()` / `port:unpin()`

`release()` hands both channels back to the engine's AI.

`face` writes `+0x1F4` only, so the engine's own VFPU rotator renders the turn. Writing the
orientation matrix instead gets clobbered the next frame.

`pin` locks XZ where the monster stands; Y is left alone, because the engine drives it to the
local floor every frame and fighting that is what makes a monster sink.

---

## 4. Things that will bite

**🔴 The tick is 2 Hz.** `lua_host`'s worker runs at 10 Hz and calls `mhfu_tick` every 5th
iteration. Nothing faster is reachable from Lua — the executor hook fires only when a *new*
action is dispatched (~0.5/s measured), so it is not a substitute clock. Every threshold in a
brain has to absorb 500 ms of monster travel. Measured off 60k logged ticks: a charge covers
**~649 units per tick**, the pursuit walk ~180. So a "stop him at 400 units" rule cannot exist;
a threshold *T* means he comes to rest anywhere in `(T-649, T]`.

**🔴 A mod must not define `mhfu_tick`.** There is one global tick and the runtime owns it.
Use `port:brain(fn)`.

**🔴 Clip ids are per PAC build.** `docs/brute_tigrex_anim_ids.txt` was labelled by filming an
earlier Brute build; on `v67_hostslots` every id shifted by one (`a1 = label - 1`). There is no
way to read a clip's *meaning* out of the file — re-derive with `tools/anim_capture.sh <pac> <a1>`
whenever the PAC changes, or render the source moveset to a captioned contact sheet
(`blender_mhfu/render_anim_clips.py` + `tools/moveset_sheet.py`, see §4b).

⚠️ **But WHICH clip is loaded is readable, and the `-1` above is not a rule.** The clip-state
block gives the answer from memory (§4b) and on the Zinogre port `a1` == the slot index **1:1**,
34/34 clip runs over three cold boots. The v67 Brute's one-off shift was a property of that
build, not of ports in general — measure it, do not assume either way.

**🔴 Pick behaviour pairs the engine ALREADY USES — this is the one that will cost you a build.**
`tools/em_moveset.py <ovl> --states` lists every `(main, sub)` the species dispatcher can reach
(231 for Tigrex). It says nothing about whether the engine ever *goes* there, and a pair it never
goes to has a handler that checks for a condition you have not created and returns immediately.

The Brute showcase ran its whole pinned loop on `(4,15)` and `(4,8)`, picked off that table
because their handlers ask for trap animations. Main 4 is the **damage-reaction** bank. On an
undamaged, untrapped monster those handlers exit at once: **411 of 411 forced moves survived
exactly one tick**, so the clip restarted from frame 0 twice a second and never played through —
which on screen is "no animation ever finishes". In ~1600 observed transitions the engine entered
either pair **zero times**.

`tools/em_state_census.py` is the check. It reads the same `framework.log` the observe-only probe
writes and reports, per pair, how long the engine HOLDS it and whether it moves the monster:

```
state     dwell    n  move/tick     a1   verdict       (2 Hz: move/tick x2 = units/second)
(2, 8)     23.7   46       1127   [15]   HOLDS + MOVES       <- the charge
(0, 7)     15.8  109          ?   [80]   HOLDS + unmeasured
(2, 1)     11.5   45         45   [15]   HOLDS + DRIFTS      <- the "held in place" move
(3, 0)     10.9  123        121   [43]   HOLDS + MOVES
(3, 6)      5.0  120        649   [47]   short — will bounce out
(4,15)        —    0          —          NEVER ENTERED       <- what the showcase used
```

Long dwell means the handler is happy to run. Never-entered means it will bounce out however good
it looks in the dispatcher table. Closing speed alone is not enough either: `(3,6)` closes
649 units/tick when the ENGINE picks it, and exits after one tick having moved ~30 when forced
from out of range — it is a close-range lunge and the target was too far.

**⚠️ And read `move/tick` as units per SECOND before you call anything still — the tick is 2 Hz.**
This report used to print `HOLDS + STATIONARY` for anything under 60 units/tick, which made
`(2,1)`'s 45 look like standing still. It is **90 units a second, in whatever direction the
monster happens to be facing**: a clip probe that held `(2,1)` continuously walked the Brute
**10 952 → 31 164 units off the map in 450 s** while the run sat waiting for him to arrive. The
verdict column now says `STILL` under 25/tick and `DRIFTS` between 25 and 60. `?` is a gap in the
sample, not a finding — the pair never occurred on two consecutive co-located ticks.

**⚠️ A scripted move ends on its own.** The runtime drops the clip latch when the engine moves
off the pair you wrote, because leaving it latched paints your animation over whatever the AI
does next — exactly the mismatch this library exists to remove. If your brain needs the monster
held in a state, re-issue on `s.move == nil`; `play()`'s own gap guard keeps that from becoming
a per-tick force.

**🔴 `engaged` is NOT full combat mode, and a swapped monster may never reach it.** `+0x5DC`
comes up when the monster has detected you and is pursuing — the `!` over its head. Played by
hand, the ported Brute showed the `!` and roamed and pursued, but the **yellow eye never
appeared next to the hunter's name**: a swapped big monster detects but does not latch combat
⛔ Not, as this guide used to say, because "the swap leaves the combat target unwired" — that was
a misread of the target-table base and is retracted. The target is wired **to the Felyne**. So
gating a brain on `s.engaged` gates on "has noticed you",
which is weaker and flickier than it sounds.

**The read for "is he after ME" is `s.targets_player`** (`+0x2F4`, the resolved combat target),
and it is worth watching but **not worth gating on**. Sampled every 1.5 s across two takes of a
swapped Brute: with him never closer than 3200 units it was **the CAT, 100 % of samples**; with
him at a median of 663 units and in the scripted loop, **CAT 44 %, PLAYER 42 %, none 14 %**. So
it oscillates several times a second up close and loses outright at range — a brain that gates on
it stutters. Log it (every showcase phase line carries `tgt=`), gate on `engaged`, and treat a
single sample of this cell as meaningless.

⚠️ A native Tigrex reads PLAYER 100 %, but that is **not** a native-vs-swap difference: the
savestate it was measured on (`tigrex_s6`) has no Felyne at all. It is cat-present vs cat-absent.

**🔴 What it usually resolves to is the FELYNE, and that is not a swap defect.** `+0x542` is a
target priority index — `0` = player, `1` = cat — and the Felyne outranks the hunter whenever it
is alive. Holding the cat at HP 0 raises the player's share of `+0x2F4` from 0–4 % to 32–100 %
(`tools/felyne_target_test.py`). ⚠️ It does not reliably hand the target over, and it does **not**
produce the yellow eye. ⛔ And **`+0x2A4` is not the cell to chase** — it reads 0 on a *native*
Tigrex that is engaged, targeting the player and mid-attack with the eye plainly visible
(`tigrex_s6`, screenshot-verified). The best current reading is that the eye means "this
monster's target is me right now" and what differs is the **duty cycle**: ~6 s on the cat, ~0.1 s
on the player, too brief to see. Note also that `dmg_experiment.Culler` cannot touch the cat — it
walks the entity registry and the Felyne is not in it (fixed address `0x090BDC40`).

**⚠️ A coordinate pin fights the engine, and the SIZE of the fight is the diagnostic.**
`port:pin()` rewrites the position at the 2 Hz tick while the engine keeps advancing it every
frame, so the lock is always undoing something. What matters is how much. Against a pursuit
state it is a tug of war the player can see: a play session logged `pin corrected` of **526–646
units on every single tick**, and on screen that is a monster sliding forward and snapping back
twice a second — which is exactly what "he floats forward and clips back" was. Against a pair the
engine actually dwells in it is ~45.

So the rule is not "never pin", it is **pin a pair that only drifts, and watch the number**. The
runtime prints a running total (`pin has corrected N ticks, M units total`); divide and compare.
If it is in the hundreds per tick you have the wrong behaviour pair, not a missing lock. And do
not expect to retire the pin altogether — the intent was to drop it once the pair underneath was
stationary, and the measurement said no pair in the census is stationary enough for a monster to
be held still for four seconds without one.

**🔴 Verify your clip ids on YOUR build before you trust them.** They are positions in the packed
PAC, and they move whenever the PAC is rebuilt. `docs/brute_tigrex_anim_ids.txt` was labelled by
filming an *earlier* Brute and the `-1` offset recorded for `v67_hostslots` is confirmed for
`a1=51` and nothing else. A wrong id is not a subtle failure: clips carry root motion on the hip
joint, so painting the wrong one over a grounded behaviour visibly lifts the monster off the floor.

`tools/clip_probe.py` films several ids in one cold boot — it holds the monster in `(2,1)`, pins
his coordinates so anything moving is the clip's own motion, and cuts the recording into one
folder per id. Filmed on `v67_hostslots` (2026-08-26): **`a1=61` and `a1=82` render the Brute high
off the ground and rotating; `a1=69` renders him low.** That is the "he flings into the air way
higher than he should" the showcase was reported with, and it is attached to specific ids.

⚠️ **It is not yet enough to say what any of them IS**, and the reason is worth knowing: a forced
pair does not correspond to one executor dispatch. One seven-tick `(2,1)` asked the executor for
a1 15, 11, 19 and 18 in turn — the handler runs a *sequence*. The probe overrode all of them, so
each window is one clip restarted several times over several different behaviours. `port:play`
now latches for **one** dispatch by default for exactly this reason; a clean single-clip capture
needs the probe to do the same.

**🔴 Decode floats with `mhfu.read_f32`, not by hand.** The `if u >= 0x80000000` idiom in most
of this repo's scripts is always true on the PSP build (32-bit `lua_Integer` wraps the literal),
so it sign-flips every value it reads. Distances survive it — negating both endpoints changes
nothing — so it stays invisible until you write a position back and teleport your monster to the
mirror image of where he stood.

**⚠️ The Lua slab is 256 KB for the whole VM** (`LUA_SLAB_BYTES` in `lua_host/mod.cpp`), prelude
and every mod included — it was 64 KB, which the port runtime plus one mod could not fit into
(`peak=80176B`). `[lua_host] VM ready` prints live and peak. Going much larger is its own
failure: 512 KB starved the section asset load and killed the game.

---

## 4b. What a port's clip vocabulary actually is, and how to check it

**The porter maps a source clip onto the host slot of the SAME INDEX**, and that
one sentence has three consequences you have to know before writing a brain.

Run the audit — it needs no emulator:

```bash
python tools/port_clip_probe.py --pac zinogre_v2.bin \
    --coverage workspace/extracted_mhp3/data_files/file_05341.bin
```

For the Zinogre port (MHP3rd `file_05341` → `zinogre_v2.bin`) it reports:

| | | |
|---|---:|---|
| **CARRIED** | 34 | source clip landed, frames **and** loop flag match exactly |
| **DROPPED** | 8 | source slots 16, 22, 27, 33, 39, 63, 66, 67 — the host Tigrex pack has no slot of that index, so they are gone |
| **FILLER** | 30 | host slots 3, 43, 45–48, 51, 52, 54, 57–60, 68–72, 74–78, 80, 83, 90, 95–98 hold a **copy of the idle clip** |

So:

1. **`a1` IS the slot index, 1:1** — verified live, 16/16 clip runs across two cold
   boots. `a1 = 10` plays the source's slot 10. There is no offset to discover
   (the Brute's `-1` was a property of *that* build, not a rule).
2. **A third of the a1 space plays IDLE.** Forcing 51 or 83 is not a failed
   override, it is a successful override onto filler — and on screen the two are
   identical. Every "the latch didn't work" reading has to rule this out first.
3. **Eight source moves are not in the build at all.** No amount of scripting
   reaches them; they need a porter that maps by meaning rather than by index.

### The clip is verifiable from memory — do not judge it by eye

`ent+0x80 + slot*0x40` is the clip-state block (`docs/agent_memory_map.md`):
`+0x10` phase, `+0x1C` end, `+0x38` clip node ptr. Two facts make it decisive:

* **`+0x38` proves provenance.** A relocate-injected PAC lives at `0x0B000000`, so a
  clip node inside that window came from *our file* and nowhere else.
* **`+0x1C` names the clip.** Clips are stored unpadded at their authored length, so
  `end` is a fingerprint — 18 of the Zinogre's lengths are ones the host Tigrex
  cannot produce, and `end = 408` can only be its slot 10.

`phase` climbing to `end` is then the whole "does it play through" question,
answered without a single screenshot. `tools/port_clip_probe.py` does all of it
and re-analyses a saved take offline with `--report-only`.

**Measured on the Zinogre port, engine-driven, no forcing:** 22/22 clip runs read
from our PAC; 14/15 reached ≥90% of their own authored length. **His clips play,
at his own timing.** ⚠️ But `speed` (`+0x14`) comes from the *action dispatch*, not
from the clip — 2.0 and 2.4 both observed — so the frame SPAN is authentic while
the RATE is the host's.

### 🔴 A clip latch alone does not hold — this is the two-channel rule again

`Port:latch(a1)` (animation only, no behaviour write) is an instrument, not a way
to script. Measured over one take: the hook substituted **10 times** and every
substitution reached the executor, but only **one** was still on screen a tick
later. The rest were overwritten within ~500 ms by the engine's own next dispatch
— which, at pair `(0,2)`, kept asking for a1 51, a *filler* slot, so the monster
visibly fell back to idle.

The one that survived is the proof the mechanism works, and it is unambiguous:

```
asked a1=10 -> +0x324 reads 10, clip node 0x0B053C90 (inside our PAC),
end = 408 frames (our slot 10, a length no other clip in either pack has),
phase climbed 4 -> 384 = 94% of it, over 7.5 s
```

⇒ **You can trigger any carried clip, and it plays its authentic length. You
cannot KEEP it with the animation channel alone** — the behaviour handler
underneath goes on choosing animations, and it wins. Holding a clip means owning
the pair under it, which is exactly what `port:play()` does and why it writes both
channels. Script with `play()`; use `latch()` only to identify clips.

## 5. Testing without an emulator

Every real test costs a cold boot, a quest, a walk and a wait for a roaming monster — about six
minutes before the first line of a brain runs, and the commonest thing to waste it is a Lua
syntax error.

```bash
docker run --rm -v "$PWD:/w" -w /w nickblah/lua:5.4-luarocks-alpine \
    lua tools/lua_dryrun/dryrun.lua <files...>
```

stubs `mhfu`, puts a monster and a hunter on an empty plane, moves the monster at the **measured**
speed for whatever behaviour pair the brain writes, and prints the brain's own log. Pass the files
**both ways round** — that is the test that the load-order bootstrap actually works.

`luac -p` from the same image is the syntax check.

---

## 6. The worked example

`brute_showcase.lua` + `tools/brute_showcase.py`: an MHP3rd Brute Tigrex replacing the Giadrome
of the 2★ *Anführer der Fleischfresser*, running a charge → pinned → struggle → break-free loop
whenever he is in the hunter's section **and** engaged, and handed straight back to his own AI
when he is not.
