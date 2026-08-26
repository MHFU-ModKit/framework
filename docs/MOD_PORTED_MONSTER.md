# Porting a monster and scripting its AI — the Lua surface

**Status (2026-08-26):** the runtime is `framework/prx/mods/lua_host/scripts/mhfu_port.lua`;
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
    pac     = "zinogre_v3.bin",
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
| `moves` | name → `{ main, sub, clip }` (or `anim = <a1>` to skip the vocabulary) |

The injector is armed **once per boot** however many times `define` runs, so hot-reloading a
mod file is safe.

### `port:brain(fn)`

`fn(s)` runs on every tick with the monster alive. `s` carries:

| field | |
|---|---|
| `ent` | entity pointer |
| `tick` | tick counter |
| `x, y, z` / `px, py, pz` | monster / player world position |
| `dist` | XZ distance between them |
| `travelled` | units moved since the last tick — the only speed signal a 2 Hz brain gets |
| `section`, `area`, `same_section` | |
| `engaged` | `entity+0x5DC` — the engine's own "combat mode entered" flag, the state the yellow eye marker reflects |
| `main`, `sub` | the live behaviour pair |
| `move` | the scripted move still running, or `nil` when it has ended |
| `pinned` | is the coordinate lock on |
| `hp`, `player_hp` | |

### `port:play(name [, min_gap])`

Writes the behaviour pair and latches the clip. Returns `false` if it declined.

Refuses to re-issue the **same** move within `min_gap` ticks (default 2). That guard is not
politeness: re-entering the executor every tick restarts the move before it ever reaches its
hitbox frames — the measured failure mode of a held `a1` force — and repeated forcing makes the
engine OR in the exhaustion bits and halt the AI outright.

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
way to read a clip's meaning out of the file — re-derive with `tools/anim_capture.sh <pac> <a1>`
whenever the PAC changes.

**⚠️ Pick behaviour pairs by measurement, not by name.** `tools/em_moveset.py <ovl> --states`
lists every `(main, sub)` for a species and the animation its handler asks for, but not what the
move *is*. The showcase's charge pair `(3,6)` was found by mining `framework.log` for
per-state closing speed — it closes 649 units/tick against 340 for the next fastest. That kind
of evidence is cheap and it is the only kind that is not a guess.

**⚠️ A scripted move ends on its own.** The runtime drops the clip latch when the engine moves
off the pair you wrote, because leaving it latched paints your animation over whatever the AI
does next — exactly the mismatch this library exists to remove. If your brain needs the monster
held in a state, re-issue on `s.move == nil`; `play()`'s own gap guard keeps that from becoming
a per-tick force.

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
