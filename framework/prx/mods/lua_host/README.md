# lua_host — embedded Lua 5.4 scripting mod (Phase 1)

Embeds a sandboxed Lua 5.4 VM in `mhfu_framework.prx` and runs an embedded Lua
"script mod" against a curated `mhfu.*` API. First step of the Lua modding
platform — see `docs/LUA_MODDING_PLATFORM_PLAN.md`.

## What it does (Phase 1)

- Allocates a **512 KB slab** (`sceKernelAllocPartitionMemory`, user partition
  = `2`) and backs Lua with a self-contained implicit-free-list allocator —
  Lua never touches the game/newlib heap.
- **Sandbox:** opens only base/table/string/math; nils
  `dofile/loadfile/load/loadstring/require/collectgarbage/raw*`. (io/os/debug
  are dead-stripped at link — never present in the binary.)
- Registers `mhfu` global: `log`, `read_u8/u16/u32`, `mem_valid`,
  `get_screen_state/area_index/quest_timer`, `entity_at/type/hp/size/alive`,
  `entities_of_type(type)->{ptrs}`, `MON_POPO/ANTEKA/TIGREX/GIADROME`.
- `lua_atpanic` handler **parks the poll thread instead of `abort()`** so a Lua
  panic can't `sceKernelExitGame` the game.
- A 2 Hz poll thread calls the embedded script's `mhfu_tick()` via `lua_pcall`.
- Demo script: logs each Popo's HP while in a stable in-area frame (`scr==17`).

## Lua API reference

The flat `mhfu.*` C bindings are wrapped by an **OO sugar layer**
(`scripts/_prelude.lua`, run before any user mod) so scripts never carry raw
addresses, struct offsets, or float-packing math. Prefer the OO API; the flat
bindings stay available for escape-hatch work.

### `mhfu.world` — global state

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

### Entity handles (`mhfu.entity.wrap(ptr)` / `mhfu.world.first/entities`)

`:valid()` `:type()` `:hp()` `:size()` `:set_size(v)` · `:pos()`→x,y,z
`:set_pos(x,y,z|vec)` · `:yaw()` `:set_yaw(v)` · `:ai_state()` `:set_ai_state(v)`
· `:engaged()` `:set_engaged(b)` · `:section()` `:set_section(v)` · `:calm()`

High-level intents (RE recipes lifted into the framework):
- `:make_visible([section])` — fix the swap/relocate render gate (`+0x29A`/`+0x638`); defaults to the player's area.
- `:force_aggro(target)` — write the full engine engage signature toward `target` (Player/Entity/`{x,y,z}`).
- `:teleport_near(target, offset)` — move to `offset` units past `target`, then `make_visible`.
- `:dist_to(target)` — XZ-plane distance.

Setters return the handle, so calls chain: `tig:set_size(0.3):make_visible()`.

### Player handle

`:pos()`→x,y,z · `:hp()` · `:area()` · `:screen_state()` · `:dist_to(ent)`

### `mhfu.mem` — raw access (escape hatch)

`read_u8/u16/u32`, `write_u8/u16/u32`, `read_f32`, `write_f32`, `valid(addr)`.
(Also still present flat as `mhfu.read_u32` etc.)

### Events (unchanged)

`on_quest_targets_building`, `on_bigmonster_spawn/death`, `on_bigmonster_action`,
`on_bigmonster_action_decided/_input`, `on_bigmonster_slot_picked`,
`on_ai_overlay_loaded`. Callback ctx still carries a raw `entity` ptr — wrap it
with `mhfu.entity.wrap(ctx.entity)` for the OO API.

Reference port using the whole surface: `scripts/tigrex_hunt.lua`.

## Status

- ✅ Verified live: the Lua mod runs and reads **live, changing** game memory
  (`screen_state`, `area_index`, entity-registry walk) through a cold boot.
- ◑ **OPEN:** reading Popo HP *in snow section 1* is blocked — the game crashes
  (`sceKernelExitGame`) on section-1 entry whenever the PRX is loaded (happens
  with lua_host alone, AND independently with `tigrex_spin` which injects a
  Tigrex that can't spawn in section 1). The demo's `scr==17` read-gate is an
  untested mitigation. Root-cause + verification is the next task. Details +
  next steps in `docs/LUA_MODDING_PLATFORM_PLAN.md` → Phase 1 RESULTS.

## Build / install

Add `lua_host` to `framework/prx/build/mods.manifest`, then `make`. `-llua -lm`
is already in `Makefile.psp` LIBS. Keep `tigrex_spin` OFF when testing popo
sections (it crashes section-1 entry independently).

Install: copy `mhfu_framework.prx` to
`<memstick>/PSP/PLUGINS/mhfu_framework/`. **Cold boot only** — loading a
savestate bypasses/breaks the plugin.

## Test

`PYTHONPATH=src python tools/smoke_lua_host.py` (cold-boot recipe), or drive into
section 1 with `tools/auto_cold_boot.py` and read `framework.log` for
`[lua_mod] … popos=N … hp=` lines.
