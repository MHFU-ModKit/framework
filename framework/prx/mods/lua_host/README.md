# lua_host — embedded Lua 5.4 scripting platform

Embeds a sandboxed **Lua 5.4** VM inside `mhfu_framework.prx` and runs Lua
"script mods" loaded from the memstick against a curated `mhfu.*` API. This is
the full Lua modding platform — Phases 0–4 of the plan
(`docs/LUA_MODDING_PLATFORM_PLAN.md`) are **done and verified live**. Drop a
`.lua` file next to the plugin, cold-boot once, then iterate with hot reload.

## What it does

- **Loads mods from the memstick** (Phase 3): every
  `ms0:/PSP/PLUGINS/mhfu_framework/mods/*.lua` is `luaL_loadbuffer`'d at boot —
  no recompile to add or edit a mod. An embedded `tigrex_spin.lua.h` is kept
  only as a safety fallback if that directory is empty (the PRX is never
  bricked).
- **Hot reload** (Phase 4): the worker thread polls each tracked `.lua`'s
  `(size, mtime)` via `sceIoGetstat` at 2 Hz and re-execs changed files into
  the **same** VM — a live edit takes effect with no cold boot. Live C hooks
  pick up the fresh closures (store_ref unrefs old + stores new).
- **OO sugar layer** (`scripts/_prelude.lua`, run before any user mod) wraps the
  flat `mhfu.*` C bindings so scripts never carry raw addresses, struct offsets,
  or float-packing math. Prefer the OO API; the flat bindings stay as an escape
  hatch.
- **AI override events** (Phase 2): the full big-monster AI override-event API
  is bound into Lua; a script's return value is threaded back into the engine
  register (`LUA_32BITS` → Tigrex ptrs round-trip).
- **Heap:** a dedicated **64 KB slab** (`sceKernelAllocPartitionMemory`) with a
  self-contained implicit-free-list allocator — Lua never competes with the game
  or framework newlib heap. (The slab was 512 KB in Phase 1; that **was** the
  snow-section-1 crash — MHFU leaves <512 KB contiguous free at runtime and each
  section load pulls its own PAC, so a big slab starved the load → the game bailed
  with `sceKernelExitGame`. Keep the slab small. Root-cause + bisect in memory
  `lua-modding-platform-phase1`.)
- **Sandbox:** opens only base/table/string/math; nils
  `dofile/loadfile/load/loadstring/require/raw*`. io/os/debug are dead-stripped
  at link (never in the binary). `lua_atpanic` parks the thread instead of
  `abort()` so a Lua panic can't kill the game.
- **Threading:** the VM is serialised by a binary semaphore (every entry into
  `g_L` — exec / poll / worker). AI override callbacks fire on the engine GAME
  thread, which **corrupts** Lua heap allocation done in its context (bisected;
  a PPSSPP per-thread quirk), so those callbacks **marshal** the request to a
  dedicated exec thread and block for the result. spawn/death (5 Hz poll) and
  `mhfu_tick()` (our 2 Hz worker) run on good contexts → they call the VM
  directly. (memory `lua-game-thread-corruption-fixed`.)

## Lua API reference

### OO API (preferred — `scripts/_prelude.lua`)

**`mhfu.world`** — global state:

| Call | Returns |
|------|---------|
| `mhfu.world.player()` | a **Player** handle |
| `mhfu.world.first(type)` | first live **Entity** of `type`, or `nil` |
| `mhfu.world.entities(type)` | array of **Entity** handles |
| `mhfu.world.area()` | area_index (map section) |
| `mhfu.world.screen_state()` | screen-state byte (17 = in area) |
| `mhfu.world.in_area()` | `true` when `screen_state == 17` |
| `mhfu.world.quest_timer()` | quest timer (frames @ 30 Hz, decrements) |
| `mhfu.world.paint_map()` | paintball all big monsters on the minimap (call ≥2 Hz) |

**Entity handle** (`mhfu.entity.wrap(ptr)` / `mhfu.world.first/entities`):

`:valid()` `:type()` `:hp()` `:size()` `:set_size(v)` · `:pos()`→x,y,z
`:set_pos(x,y,z|vec)` · `:yaw()` `:set_yaw(v)` · `:ai_state()` `:set_ai_state(v)`
· `:engaged()` `:set_engaged(b)` · `:section()` `:set_section(v)` · `:calm()`

High-level intents (RE recipes lifted into the framework):
- `:clone()` — deep-copy this entity into a new swarm member (§49 `entity_clone`:
  rebases self-ptrs, splices the `+0x1C4` update chain, takes a free registry
  slot). The clone shares the resident model/overlay/species block — only the
  ~31 KB entity struct is duplicated. Clone scratch comes from the `memory=64`
  extra RAM (`0x0A800000+`); the managed partition has <512 KB free.
- `:make_visible([section])` — fix the swap/relocate render gate (`+0x29A`/`+0x638`); defaults to the player's area.
- `:force_aggro(target)` — write the full engine engage signature toward `target` (Player/Entity/`{x,y,z}`).
- `:teleport_near(target, offset)` — move to `offset` units past `target`, then `make_visible`.
- `:dist_to(target)` — XZ-plane distance.

Setters return the handle, so calls chain: `tig:set_size(0.3):make_visible()`.

**Player handle:** `:pos()`→x,y,z · `:hp()` · `:area()` · `:screen_state()` · `:dist_to(ent)`

**`mhfu.mem`** — raw access (escape hatch): `read_u8/u16/u32`,
`write_u8/u16/u32`, `read_f32`, `write_f32`, `valid(addr)`.

### Flat `mhfu.*` bindings (the C surface the OO layer wraps)

- **memory:** `read_u8/u16/u32`, `write_u8/u16/u32`, `read_f32`, `write_f32`,
  `mem_valid`.
- **world:** `get_screen_state`, `get_area_index`, `get_quest_timer`,
  `get_player_hp`, `player_pos`, `paint_map`.
- **input:** `buttons` (read the pad).
- **camera:** `freecam`, `freecam_active`, `cam_snap_disable`,
  `cam_freeze_player`, `cam_village_unlock`, `cam_config`, `cam_eye`.
- **entity:** `entity_at`, `entity_type`, `entity_hp`, `entity_size`,
  `entity_set_size`, `entity_alive`, `entity_pos`, `entity_set_pos`,
  `entity_yaw`, `entity_set_yaw`, `entity_ai_state`, `entity_set_ai_state`,
  `entity_engaged`, `entity_set_engaged`, `entity_calm`, `entity_section`,
  `entity_set_section`, `entity_make_visible`, `entity_force_aggro`,
  `entity_clone`, `entities_of_type`.
- **quest:** `quest_has`, `quest_monster_count`, `quest_first_monster`,
  `quest_replace_monster`, `quest_add_monster`.
- **overlay:** `load_relocated_overlay` (relocate a 2nd different-family AI
  overlay; §39–48).
- **AI:** `action_ptr_for`.
- **effects (VFX):** `spawn_effect(ent, id, bone)` -> handle (0 = not spawned),
  `bone_pos(ent, bone)` -> x,y,z. A monster's effects are emitted by CODE in its
  species overlay, so a ported monster inherits the host's and can only get its
  own if a mod fires them — this is that primitive. 🔴 **Call it only from inside
  an override callback**: it allocates from the effect manager, and the same call
  from a native `ai_step` prefix kills the emulator within a second (measured; a
  dry run of the same driver was clean, so it is the call from that context, not
  the code). ⚠️ A zero handle is usually the section gate
  (`entity+0x29A == current section`), not a bad id. Bone numbers are the LOADED
  skeleton's. → `docs/EFFECTS_AND_VFX.md`, `tools/em_effects.py`.

### Events

`on_quest_targets_building`, `on_bigmonster_spawn`, `on_bigmonster_death`,
`on_ai_overlay_loaded`, `on_bigmonster_slot_picked`, `on_bigmonster_action_input`,
`on_bigmonster_action_decided`, `on_bigmonster_action`. Callback ctx carries a
raw `entity` ptr — wrap it with `mhfu.entity.wrap(ctx.entity)` for the OO API.
The production big-monster action-force seam is `on_bigmonster_action` (entry
detour on the executor `0x09AC5228`; rewrite the `a1` action id → the engine fans
it coherently to all body slots — see `docs/AI_SCRIPTING_ENGINE.md` §32k).

## Demo scripts (`scripts/`)

| Script | What it does |
|--------|--------------|
| `megatigrex.lua` | **Headline.** On any quest with exactly one big monster, spawn a swarm of 10 mini-Tigrex (`entity_clone` §49) and paint them on the map. Shepherds orphaned clones across section transitions. (~2 of the swarm deal player damage — the engine's combat registration ceiling; see CLAUDE.md §49.) |
| `tigrex_spin.lua` | SPIN-lock demo: Giadrome→Tigrex swap + coherent action force via `on_bigmonster_action` + freeze-gate `+0x4B8` clear + auto-paint. Reference for the action-force seam. |
| `tigrex_hunt.lua` | Tigrex roams to your section + force-aggros after 30 s. Uses the whole OO surface. |
| `tigrex_section1.lua` | Spawn-native-then-relocate a big monster into snow section 1. |
| `tigrex_aggro.lua` / `tigrex_invest.lua` / `tigrex_ontop.lua` | aggro / investigation / co-location test rigs. |
| `freecam.lua` | Toggleable orbit fly-cam + independent free-look (quest + village). |
| `zinogre_fx.lua` | Effect-library sweep on the ported Zinogre: fires each id at a bone and logs the handle. Driver `tools/port_effect_probe.py`. |

`*.lua.h` files are the embedded-fallback headers generated from each script by
`tools/embed_lua.py` (octal-string header). Regenerate after editing a script
you want available as a fallback.

## Build / install

`lua_host` is in `framework/prx/build/mods.manifest`; `-llua -lm` is already in
`Makefile.psp` LIBS. `make` from `framework/prx`. Keep the C `tigrex_spin` mod
OFF — it claims the same vt[8] chain as the Lua scripts.

Install: copy `mhfu_framework.prx` to `<memstick>/PSP/PLUGINS/mhfu_framework/`
and your scripts to `.../mhfu_framework/mods/*.lua`. **Cold boot once** — plugins
load only on cold boot (a savestate bypasses the plugin loader). After that,
hot reload covers script edits.

**For `entity_clone` / the megatigrex swarm:** add `memory = 64` to the plugin's
`plugin.ini` `[options]` section. This grows PPSSPP's raw RAM to `0x0C000000`;
clone scratch + relocated overlays live in the otherwise-unused extra region
`[0x0A000000, 0x0C000000)`. Without it the swarm has nowhere to allocate.

## Status

✅ Phases 0–4 done and verified live: sandboxed VM reads/writes live game memory
through a cold boot; AI overrides round-trip (Tigrex spin-lock); mods load from
the memstick; hot reload of a memstick `.lua` applies while the game runs.

The Phase-1 snow-section-1 crash is **solved** (slab 512 KB → 64 KB — it was
memory exhaustion, not the read gate). Remaining caveat is engine-side, not
platform-side: a cloned big monster has no engine manager, so only ~2 of a swarm
deal player damage (the §48 combat-registration ceiling) and clones must be
shepherded across section transitions (megatigrex does this).

## Test

`PYTHONPATH=src python tools/smoke_lua_host.py` (cold-boot recipe), or drive into
a quest with `tools/auto_cold_boot.py` and read `framework.log` for `[lua_mod]`
lines.
