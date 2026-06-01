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
