# Lua Modding Platform — Investigation + Plan

Status: PROPOSAL (2026-05-31). Awaiting go/no-go on next steps.
Goal: let mod authors write **runtime-loaded Lua scripts** against a curated,
safe subset of the existing C++ framework API — instead of compiling C++
descriptor mods. Multiple scripts loaded by one PRX. Easier authoring, harder
to crash the engine, no asm / pointer math.

---

## 1. Online investigation — findings

(Full report archived in session; sources at bottom.)

### Engine choice

| Engine | Stripped bin | Heap @ start | PSP/MIPS port | In pspdev | C std | Verdict |
|--------|-------------|--------------|---------------|-----------|-------|---------|
| **Lua 5.4.6** | ~115–185 KB | ~10–28 KB | YES (default) | `lua54` | C89+flags | **PICK** |
| Lua 5.1.5 | ~100–175 KB | ~8–25 KB | YES (verified) | `lua51` | C89 | fallback |
| LuaJIT | — | — | **NO (port broken)** | no | — | ruled out |
| Squirrel 3 | ~130 KB | ~20 KB | yes | `squirrel` | C++03 | backup only |
| AngelScript | ~700 KB | ~63 KB | yes | yes | C++03 | too big |
| Wren / Berry / Janet / PocketPy | 40 KB–850 KB | — | unverified | no | C99/C11 | no PSP ecosystem |

**Decision: Lua 5.4.6** (5.1.5 fallback if 5.4 hits a psp-gcc snag).
Already packaged in pspdev (`psp-pacman -S lua54` → `liblua.a` + headers).
Generational GC reduces fragmentation over a long session. Table syntax is
the most familiar to hobbyist modders. LuaJIT is a hard no on Allegrex
(PSP port marked "not working", N32 vs O32 ABI, no W^X pages).

### Mandatory PSP build flags (all Lua versions)

```
-DLUA_USE_C89        # psp-gcc lacks C99 long long → forces long+double number path
-DLUA_C89_NUMBERS    # disables snprintf number-formatting path
-DLUA_32BITS         # lua_Number=float, lua_Integer=int32 — Allegrex has NO hw double
-DLUA_MAXSTACK=1000  # value stack cap (1000*12B = 12KB) — fits PSP thread stack
-DLUA_MAXCCALLS=50   # recursion cap — prevents 16KB thread-stack overflow
-Os -G0 -mno-explicit-relocs   # -Os small; -G0 + -mno-explicit-relocs dodge the
                               # PRX GP-register / newlib bug
```

`-DLUA_32BITS` is non-negotiable: without it every number is a software-emulated
double (~5–10× slower per op, 2× memory). Allegrex FPU is single-precision only.
VFPU is not used by Lua core.

### Hard constraints we must respect

1. **Memory budget is tiny.** MHFU running leaves on the order of **1 MB**
   contiguous free user RAM. → Pre-reserve a **fixed slab** (start 512 KB) via
   `sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, ...)` at PRX init and
   back Lua with a **custom `lua_Alloc`** (TLSF or dlmalloc over the slab). Never
   let Lua call the game's `malloc`. Instrument with `lua_gc(LUA_GCCOUNT)`.
2. **No second plugin PRX.** PPSSPP wedges MHFU with two plugin PRXes (already
   known — see README "Why one PRX"). → Lua interpreter must be **statically
   linked into `mhfu_framework.prx`**, not loaded as its own module.
3. **PRX stdio is broken** (GP-register/newlib bug). → Load scripts with
   `sceIoOpen`/`sceIoRead` + `luaL_loadbuffer`, **never** `luaL_loadfile`/`fopen`.
4. **No exceptions/RTTI in core today** (`-fno-exceptions -fno-rtti`). Lua's
   default error path is `setjmp/longjmp` (C, fine) — keep it; do NOT compile
   Lua as C++ (would want exceptions). Register `lua_atpanic` so an unhandled
   Lua error logs + recovers instead of `abort()`-ing the game.
5. **Microsecond budget in hot hooks.** `ai_step` is per-frame per-entity (~30 Hz);
   `action_*` is ~0.5 Hz. One `lua_pcall` is ~1–5 µs on 333 MHz Allegrex — fine,
   but: pre-resolve the registry ref once, avoid string formatting in hot
   callbacks, step the GC manually (`LUA_GCSTEP`) once per ~5 frames OUTSIDE the
   hook rather than running incremental GC inside the engine call stack.

### Sandboxing

- Open only `base`, `table`, `string`, `math`. **Do not** open `io`, `os`,
  `debug`, `package`, `coroutine`, `utf8`.
- Nil out dangerous globals (`dofile`, `loadfile`, `load`, `loadstring`,
  `require`, `collectgarbage`, raw* , `setfenv/getfenv`).
- Each mod script runs in its own `_ENV` sandbox table (5.4 `load(chunk,name,"bt",env)`)
  → no cross-mod global pollution; reload = swap the env table.
- Every C binding that takes an address **validates the range** before deref
  (we already have `mhfu_mem_valid`). The Lua side never sees a raw pointer it
  can abuse outside the curated API.

### Hot reload

Achievable: keep each mod in its own env table + a list of its registered
callback refs. Reload = `luaL_unref` all its refs, drop its env, `lua_gc`
collect, re-`luaL_loadbuffer` + `pcall`. Trigger via a watched flag / file
mtime poll, or a debug command. (The C++ static-mod path has no hot reload;
this is a genuine new capability.)

### Bytecode option

`luac -s` on PC → ship `.lc`. Lets us **strip the parser** from the PRX
(`llex/lparser/lcode` ≈ −16 KB) and removes source-text attack surface.
Bytecode is **version-locked** to the linked Lua. Recommendation: support
**both** source `.lua` (dev convenience) and `.lc` (shipping) initially; revisit
parser-strip later if size bites.

---

## 2. Current framework state (what Lua binds to)

One PRX (`mhfu_framework.prx`), light-C++ core (`src/core/*.cpp`), public SDK
under `include/mhfu/*.h`, mods = descriptor TUs via `MHFU_MOD(...)` selected in
`build/mods.manifest`. All public API is `extern "C"` — **ideal for Lua FFI-style
binding** (no name mangling, POD ctx structs).

API surface already stable enough to expose to Lua:

- **memory.h**: `read/write_u8/u16/u32/f32`, `mem_valid`, named getters
  (`get_screen_state`, `get_area_index`, `get_quest_timer`, `get_player_hp`,
  `get_sharpness_current`).
- **entity.h**: `entity_at`, `entities_of_type`, `entity_type/hp/size/pos/yaw/
  ai_state/engaged`, `set_size/set_pos/set_yaw/set_ai_state/set_engaged`,
  `entity_calm`, `entity_is_alive`.
- **monster.h**: `species_set_detection`.
- **quest.h**: `quest_current/has/monster_count/replace_monster/add_monster`.
- **ids.h**: `MON_POPO/ANTEKA/TIGREX/GIADROME`.
- **events.h** (fan-out, observe): quest_beginning, quest_entered,
  map_section_entered, monster_spawned, quest_targets_building.
- **ai.h** (priority-chain, override-capable): `bigmonster_action_input`
  (force action — preferred), `action_decided` (observe/sanitise),
  `slot_picked` (redirect body-part slot), `ai_step` (observe), `spawn`/`death`,
  plus `action_ptr_for` (tigrex ptr cache), `entity_is_big_monster`.

This is exactly the set a Lua mod wants. The override events return values —
which maps cleanly onto Lua callback return values.

### Gaps the Lua layer exposes / must handle

- No filesystem-mod-discovery yet (static manifest only) — Lua needs a
  `sceIoDopen` scan of a mods dir. This is also the seed of README "approach B".
- AI override callbacks run **inside the engine call stack** at high frequency.
  Calling Lua there is allowed but must be cheap + must not error fatally.
- Event ctx are POD structs → bind as Lua tables (copy fields in) OR lightweight
  userdata with accessor metamethods. Tables are simpler; start there.

---

## 3. Proposed architecture

```
mhfu_framework.prx
├── src/core/*            (unchanged)
├── liblua.a              (static, pspdev lua54, 32-bit build)   ← NEW dep
└── mods/lua_host/        (NEW mod, MHFU_MOD descriptor)
    ├── mod.cpp           init: slab + lua_State + sandbox + bind + scan dir
    ├── lua_alloc.c       TLSF over sceKernelAllocPartitionMemory slab
    ├── lua_bind.c        registers mhfu.* C functions into the VM
    ├── lua_events.c      C event/AI callbacks → fan out to stored Lua refs
    └── lua_loader.c      sceIo scan + luaL_loadbuffer + per-mod env + reload
```

The Lua host is **just another mod** in the manifest. Ship it enabled; it then
loads N Lua scripts from the memstick at runtime. Static C++ mods and Lua mods
coexist.

### Lua-facing API shape (mod author writes this)

```lua
-- ms0:/PSP/PLUGINS/mhfu_framework/mods/calm_tigrex.lua
mhfu.on("monster_spawned", function(c)
    if c.monster_type == mhfu.MON_TIGREX then
        mhfu.species_set_detection(mhfu.MON_TIGREX, 0.0)
        mhfu.entity_calm(c.entity_ptr)
    end
end)

-- override: force tigrex SPIN whenever the engine offers a cached action
mhfu.on_action_input("TIGREX", function(c, input)
    if c.monster_type == mhfu.MON_TIGREX then
        return 0x0413   -- TIGREX_VT8_INPUT_SPIN
    end
    return input        -- abstain
end)
```

- `mhfu.on(event, fn)` — fan-out events; ctx passed as a table.
- `mhfu.on_action_input / on_action_decided / on_slot_picked` — override chain;
  Lua return value is forwarded into the engine (return the input to abstain).
- `mhfu.read_u32(addr)` etc., `mhfu.entity_*`, `mhfu.quest_*`, `mhfu.MON_*`,
  action-id constants table (`mhfu.TIGREX_VT8_INPUT.SPIN`).
- All memory accessors range-checked in C; out-of-range → nil + log, never crash.

### C↔Lua dispatch

- On `mhfu.on(...)`: `luaL_ref` the function into the registry, store
  `(event_id, ref, owning_mod)` in a C table.
- The Lua host registers ONE C callback per framework event (via existing
  `mhfu_on_*`). When it fires, it iterates stored Lua refs for that event,
  pushes a ctx table, `lua_pcall`s. For override events the host returns the
  chained Lua return into the engine.
- One shared `lua_State` for all scripts (simpler GC/heap accounting); per-mod
  isolation via `_ENV`. (Per-state-per-mod is possible later if isolation needs
  grow, at more heap cost.)

---

## 4. Phased plan

**Phase 0 — Spike (de-risk the unknowns). ✅ DONE 2026-06-01. PASS — GREEN-LIGHT.**
- See `## Phase 0 RESULTS` below. All three unknowns retired with live numbers.
- Spike source kept under `framework/prx/spike_lua/` (reproducible).

**Phase 1 — Static host, embedded script. ✅ DONE 2026-06-02. PASS.**
- `mods/lua_host/mod.cpp` shipped; a sandboxed Lua mod runs in
  `mhfu_framework.prx` and reads live game memory — **including Popo HP in snow
  section 1** (verified: 2 popos, `hp=102`). The section-1-entry crash was a
  memory-budget bug (512 KB slab > free contiguous RAM); fixed by a 64 KB slab.
  See `## Phase 1 RESULTS` below.

**Phase 2 — Events + AI override binding. 🔄 BUILT 2026-06-02, awaiting HITL.**
- Bound the AI override chain into Lua (`mhfu.on_bigmonster_action_decided` /
  `_action_input` / `_slot_picked`, `on_ai_overlay_loaded`, `on_bigmonster_spawn`
  / `_death`, `on_quest_targets_building`) + helper bindings (`write_u8/16/32`,
  `entity_set_size`, `quest_has`, `quest_replace_monster`, `action_ptr_for`).
  Lua callback return value is threaded back into the engine register.
- Ported `tigrex_spin` C mod → `mods/lua_host/scripts/tigrex_spin.lua` (1:1).
- `lua_atpanic` + per-callback `pcall` error logging; VM serialised by a binary
  semaphore (game-thread overrides vs poll-thread spawn/death).
- See `## Phase 2 RESULTS` below.

**Phase 3 — Runtime loading from memstick. ✅ DONE 2026-06-03. PASS (HITL).**
- `sceIoDopen` scan of `ms0:/PSP/PLUGINS/mhfu_framework/mods/*.lua`, `sceIoRead`
  into a 48 KB BSS scratch buffer, `luaL_loadbuffer` + `lua_pcall`, per-file
  error isolation. Embedded `tigrex_spin.lua.h` kept ONLY as a fallback when the
  dir is empty/absent (never bricks the PRX). `mod.cpp` `load_lua_dir()` /
  `load_lua_file()`. tigrex_spin now loads from the on-disk `.lua` — verified
  live: `[lua_host] loaded mod tigrex_spin.lua (5436B)` + `1 mod(s) loaded`
  (5436 B == file size; NOT the fallback), then `ANGRY_SPIN -> LOCKED`, then the
  expected AI-brittleness crash on the natural spin.
- NOT YET DONE (deferred to Phase 4/5): per-mod `_ENV` isolation (all mods
  currently share one global env + one `lua_State`); deterministic sort order
  (relies on dir-read order); `.lc` bytecode. The residual game-thread
  `action_input err: attempt to call a boolean value` is the known game-thread
  Lua-corruption bug (separate from Phase 3).

**Phase 4 — Hot reload. ✅ DONE 2026-06-03. PASS (HITL, live).**
- Worker thread (2 Hz) polls each memstick `.lua`'s (size, mtime) via
  `sceIoGetstat`; on change it takes the VM lock and re-execs the file in the
  SAME `lua_State`. The mod's `mhfu.on_*(fn)` calls run again and `store_ref()`
  unrefs the old handler + stores the new — the framework's C event hooks are
  already installed and keep dispatching, now into the fresh closures. So a
  live edit takes effect with NO hook reinstall and NO cold boot. `lua_gc`
  after each reload reclaims the old closures. `mod.cpp`: `hot_reload_scan()`,
  `prime_tracked()`, `track_set()`, `tracked_t g_tracked[16]`.
- VERIFIED live (game in village, edit driven from host shell): appending a
  comment 5436B→5486B fired `[lua_host] hot-reload tigrex_spin.lua ...` →
  re-exec `registered` line → `loaded mod tigrex_spin.lua (5486B)` (new size =
  edited file re-read) → `hot-reloaded 1 mod(s)` with `live` dropping
  23080B→19216B (GC). Repeatable: restoring to 5436B fired a 2nd reload.
  PPSSPP surfaces the host file's changed mtime/size to `sceIoGetstat`.
- NOT done (deferred): GC pacing via `LUA_GCSTEP` (currently a full
  `GCCOLLECT` only on reload, which is fine — reloads are rare); author docs +
  example mods. Per-mod `_ENV`/state isolation and unregister-on-delete still
  Phase 5 (one shared env/`lua_State`; module `local` state resets on reload —
  intended; deleting a `.lua` leaves its handlers registered).

**Game-thread Lua-corruption bug — ✅ FIXED 2026-06-03 (v0.4). PASS (HITL).**
Long-standing bug: the AI override callbacks (`action_input` / `action_decided`
/ `slot_picked` / `overlay_loaded` / `quest_targets_building`) fire on the
engine's GAME thread (AI-tick), and Lua HEAP ALLOCATION done in that thread's
context corrupted — a fresh table's node array collapsed so every key read back
as the last value (`tostring`→"call a number value", concat→"call a boolean",
ctx tables → all fields = last). Surfaced as `action_input err: attempt to call
a number/boolean value`.
- ROOT CAUSE bisected exhaustively (hot-reload + a `probe.lua` ladder + C
  instrumentation). RULED OUT: C-stack overflow (30 KB headroom), FPU/COP1
  (float math fine on the game thread), `$gp` (0 GPREL relocs in liblua AND our
  objects), GC (corrupts with `GCSTOP`), allocator/region (corrupts with the
  slab AND newlib `malloc`; no stack overlap), write/read path (writing a
  pre-allocated table on the game thread is fine). Decisive control: the SAME
  inline table test runs **always-OK on the worker thread and always-CORRUPT on
  the game thread**, interleaved in real time. Conclusion: a per-thread
  emulation quirk — Lua's table-construction code misbehaves only in the
  engine's AI-tick thread context; not fixable at the Lua source.
- FIX: game-thread override callbacks NEVER touch the VM. They marshal the
  request to a dedicated **exec thread** (a proven-good context) via two binary
  semaphores (`g_req_sema`/`g_resp_sema` + `volatile lua_req_t g_req`) and block
  for the result; the exec thread runs the Lua handler and threads the return
  back into the engine register. spawn/death (poll thread) and `mhfu_tick`
  (worker) already run on good threads → direct. `mod.cpp`: `exec_thread()`,
  `marshal()`, `dispatch_input/decided/slot/overlay/quest()`.
- VERIFIED live on the real Tigrex (`type=0x4B`): every game-thread callback now
  passes the full stress — fresh table build + `tostring` + concat +
  `string.format` + ctx table — all `true`, real engine `vt8_input`s flowing, no
  errors, no freeze. Workaround constraints in `tigrex_spin.lua` (positional
  args, `string.format`-not-concat) are no longer required.

**Phase 5 (optional/later).** Parser-strip + bytecode-only shipping; per-mod
isolated `lua_State`s; richer userdata-with-metatable ctx (now unblocked by the
marshal fix); a Lua API for the quest-injection helpers; expose action-id enum
tables generated from `ai_actions.h`.

Total core (Phases 0–4): ~4–6 focused days, sequential, each phase
independently testable on PPSSPP.

---

## 5. Risks / open questions

| Risk | Severity | Mitigation |
|------|----------|-----------|
| 512 KB slab too small for real mods | High | Measure in Phase 0; tune slab; cap script complexity; strip parser |
| psp-gcc snag building 5.4 | Med | Fall back to 5.1.5 (more battle-tested on PSP) |
| Lua error inside per-frame AI hook stutters/crashes | Med | `pcall` every callback; `atpanic` recover; budget-step GC outside hook |
| setjmp/longjmp interaction with light-C++ core frames | Med | Outermost C frame at the hook boundary; no C++ dtors between setjmp and throw |
| Per-frame Lua cost on 333 MHz | Low–Med | Pre-resolve refs; no alloc/format in hot path; observe-only ai_step gated |
| Bytecode version drift | Low | Ship matching `luac`; embed version check in loader |
| Approach-B PRX co-load still blocked | N/A | Lua host is statically composed — sidesteps it entirely |

Biggest unknown = **heap headroom**. Phase 0 must produce a real number before
committing to Phases 1+.

---

## 6. Decision points for the user

1. **Go/no-go on Phase 0 spike?** (cheapest way to retire the heap + build risk.)
2. **Lua 5.4 vs 5.1?** Recommend 5.4, accept 5.1 fallback after Phase 0.
3. **Source `.lua` + bytecode `.lc`, or bytecode-only?** Recommend both first.
4. **One shared `lua_State` (recommended) vs per-mod state?**
5. **Keep C++ descriptor mods as a first-class path too** (recommended — power
   users / bleeding-edge RE), with Lua as the easy default?

---

## Phase 0 RESULTS (2026-06-01) — PASS, proceed to Phase 1

Spike at `framework/prx/spike_lua/` (EBOOT `main.c` = runnable heap test;
`prx_link_test.cpp` + `Makefile.prx` = PRX-regime link test). Built in the
`pspdev/pspdev:latest` Docker image; EBOOT run live on PPSSPP 1.20.3.

**Toolchain reality (better than the research assumed):**
- pspdev ships **`lua54` 5.4.6-4 prebuilt and already installed** in the image
  (`/usr/local/pspdev/psp/{include/lua.h, lib/liblua.a}`). No source build needed.
- pspdev **pre-patched `luaconf.h` with `#define LUA_32BITS 1`** → the prebuilt
  lib is already 32-bit. Confirmed live: `sizeof(lua_Number)==4`,
  `sizeof(lua_Integer)==4`. No need to hand-set `LUA_32BITS`/`LUA_USE_C89`/
  `LUA_C89_NUMBERS` — just `#include "lua.h"` and link `-llua -lm`.

**(A) Runs on Allegrex + heap demand (live on PPSSPP):**
```
after newstate:    live=  2875 B    (~2.8 KB bare VM)
after 4 safe libs: live=  8105 B    (~8 KB sandboxed baseline: base/table/string/math)
script peak:       peak= 11822 B    (~12 KB: 1000-iter loop + 50-str table + C-fn call)
after full GC:     live=  8132 B    (~8 KB steady floor)
after lua_close:   live=     0 B    (no leak)
C->Lua binding:    mhfu_probe() -> 0xDEAD   (curated C function callable from Lua)
```
→ **Heap is a non-issue.** A whole sandboxed VM costs ~8 KB; a representative mod
peaks ~12 KB. A 128 KB slab fits ~10 such mods; 512 KB fits dozens. The
pessimistic ~1 MB free-RAM budget is over-satisfied by ~100×.

**(B) Links into a PRX under the framework's light-C++ regime:**
- `prx_link_test.cpp` compiled with the framework's EXACT flags
  (`-std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics`) and linked
  **with NO `-lstdc++`** → links clean, produces a valid `.prx`. **Lua pulls in
  zero C++ runtime.** (A benign "stubs out of order" import-order warning came
  from the spike's duplicated lib list; the framework's controlled link order in
  `Makefile.psp` avoids it.)

**(C) Code size cost:** Lua + spike glue adds **~238 KB `.text`** over an empty
EBOOT baseline (123,988 → 362,532 B). This is the real cost to watch — but it's
*code* (loaded once into the user partition), not the scarce heap. PSP user
partition has room; +238 KB to the PRX image is tolerable. Can be trimmed later
by stripping the parser (bytecode-only) if needed.

**(D) Sandbox proven at the binary level:** `psp-nm` confirms `luaopen_io`,
`luaopen_os`, `luaopen_debug`, `luaopen_package`, `luaopen_coroutine` are
**dead-stripped — not present in the binary at all** (only the 4 safe openers
linked). A script cannot reach `os.execute` because the code isn't there. This
is stronger than runtime nil-ing of globals (which we'll still do for `load`,
`dofile`, etc. that live inside base).

**Verdict:** No deal-breakers. Lua 5.4.6 (pspdev prebuilt, 32-bit) embeds
cleanly, runs on Allegrex, costs ~8–12 KB heap per mod and ~238 KB one-time code,
needs no C++ runtime, and sandboxes at link time. **Proceed to Phase 1.**

Reproduce:
```bash
cd framework/prx/spike_lua
docker run --rm -v "$(pwd)":/work -w /work pspdev/pspdev:latest make            # EBOOT.PBP
docker run --rm -v "$(pwd)":/work -w /work pspdev/pspdev:latest make -f Makefile.prx  # .prx link test
# run EBOOT.PBP on PPSSPP → writes ms0:/lua_spike.log (memstick root)
```

## Phase 1 RESULTS — PASS

**RESOLVED 2026-06-02 — Popo HP read live in section 1.** The Lua mod logged:
```
[lua_mod] tick=179 scr=17 area=99 popos=2 | #1@090C28E0 hp=102 sz=0.80  #2@090C7340 hp=102 sz=1.10
```
All Phase-1 goals met: sandboxed Lua mod runs in the PRX, reads live game memory,
walks the entity registry, reads each Popo's HP in snow section 1.

**Root cause of the section-1-entry crash = MEMORY EXHAUSTION (not stack, not
reads, not a Lua panic).** The free-mem log told the story:
```
[lua_host] free mem before slab: max=490496 total=709120   (~479 KB contiguous,
                                                             ~692 KB total free)
[lua_host] free mem after 64KB slab: max=424704 total=643328
```
With MHFU running, only **~479 KB *contiguous*** free exists. The original
**512 KB** `sceKernelAllocPartitionMemory` slab either failed or left too little
for the snow section-1 interior asset load (each section swaps in its own
model/anim PAC), so the game's resource loader bailed with `sceKernelExitGame`
exactly at the 98→99 transition (one frame rendered, then exit). Basecamp was
already resident so it survived. **Fix: 64 KB slab** (Lua needs ~15 KB live;
4× headroom) — returns ~448 KB to the game; section 1 now loads and the mod runs.

Bisection that nailed it (each a separate cold-boot run):
1. plugin **disabled** → section 1 fine ⇒ it's our framework, not the game/quest.
2. **core-only** PRX (noop mod) → section 1 fine ⇒ not the framework core.
3. **lua_host, 512 KB slab** → crash ⇒ lua_host.
4. **lua_host, 64 KB slab** → section 1 fine + Popo HP logged ⇒ memory budget.

Lesson for the platform: **keep the Lua slab small** (the Phase-0 "512 KB fits
dozens of mods" was true for Lua's heap need but ignored that MHFU leaves <512 KB
*contiguous* at runtime and section loads need it). Size the slab to measured
live use + margin; consider freeing/relocating it, or allocating lazily, if
future mods need more.

### (original Phase-1 build notes — 2026-06-01)

Shipped `framework/prx/mods/lua_host/mod.cpp` — a descriptor mod that embeds a
sandboxed Lua 5.4 VM in `mhfu_framework.prx` and runs an embedded Lua "script
mod" against a curated `mhfu.*` API. Built + installed; verified live on PPSSPP.

**What landed:**
- **Dedicated slab allocator.** `sceKernelAllocPartitionMemory(2 /*USER*/, …,
  PSP_SMEM_Low, 256 KB)` + a self-contained implicit-free-list allocator
  (malloc/free/realloc + forward coalescing) wired as Lua's `lua_Alloc`. Lua
  never touches the game's allocator or the newlib heap. Live: `live≈15 KB`,
  `peak≈15.7 KB` for the demo mod — a hair above the Phase-0 bare number
  because the demo script + string lib are resident. 256 KB slab = ~16× headroom.
- **Sandbox.** Opens only base/table/string/math; nils `dofile/loadfile/load/
  loadstring/require/collectgarbage/raw*`. (io/os/debug dead-stripped at link —
  Phase 0.)
- **Bindings** (`mhfu` global table): `log`, `read_u8/u16/u32`, `mem_valid`,
  `get_screen_state/area_index/quest_timer`, `entity_at/type/hp/size/alive`,
  `entities_of_type(type)->{ptrs}`, plus `MON_POPO/ANTEKA/TIGREX/GIADROME`.
- **Poll thread** at 2 Hz calls the script's `mhfu_tick()` via `lua_pcall`
  (errors logged, never fatal).
- **Build wiring.** `-llua -lm` appended to `LIBS` in `Makefile.psp` (after the
  SDK stub libs); `lua_host` added to `build/mods.manifest`. The pre-existing
  benign "stubs out of order" import warning rises 2→4 (Lua pulls more libc
  imports) — same class the framework already ships with; PRX loads fine.

**Live verification (cold boot — plugins load ONLY on cold boot; `--state`
bypasses them, confirmed):**
```
[lua_host] VM ready: lua_Number=4B live=15368B peak=15672B tick=1
[mods] 'lua_host' v0.1 active
[lua_mod] scr=0  area=8   popos=0     <- reads change frame-to-frame as the
[lua_mod] scr=4  area=2   popos=0        game advances through boot…
[lua_mod] scr=2  area=2   popos=0
[lua_mod] scr=1  area=0   popos=0
[lua_mod] scr=22 area=21  popos=0
[lua_mod] scr=17 area=98  popos=0     <- in snow basecamp
```
The Lua script reads live, **changing** game memory (`screen_state` @0x08A8CA48,
`area_index` @0x08B0C7DC) every tick via both raw `mhfu.read_u8/u16` and the
named getters, and walks the entity registry (`entities_of_type`). This proves
"a Lua mod executes and reads live game memory" end-to-end.

**NOT YET captured: actual Popo HP — OPEN, blocked by a section-1-entry crash.**
The popo path (`entities_of_type(MON_POPO)` → `entity_hp`) is in the demo and
would fire the instant popos exist, but **the game crashes (`sceKernelExitGame`)
on entry into snow section 1 (area 99) every time the framework PRX is loaded** —
across 4 attempts (cold-boot walker ×3 + a user-driven manual run). Each run
reached area 99 for ~1 tick with `popos=0`, then the game exited before popos
spawned. Findings while chasing it:
* **`tigrex_spin` is one independent cause** and must stay OFF for popo tests:
  its quest hook replaces the quest Giadrome with a Tigrex (`[quest] replaced
  0x4d -> 0x4b`), and a big monster **cannot spawn in section 1** (no
  big-monster spawn tile → the long-known *crash15* class). Confirmed live in
  the log right before a crash.
* **But it is NOT only tigrex_spin** — a rebuilt **lua_host-ONLY** PRX (1
  descriptor, no tigrex) *also* crashed on section-1 entry (`tick=144 scr=1
  area=99` → dead). So the framework/lua_host presence itself perturbs the
  section-1 load. `sceKernelExitGame` is a *clean* exit (abort/quit), not a wild
  branch.
* Lua-panic was ruled out as the trigger: a `lua_atpanic` handler that parks the
  poll thread instead of `abort()`-ing was added, and the crash still happened —
  so it is not the Lua VM aborting.
* **PRIME hypothesis (unverified — try FIRST next session): poll-thread stack
  overflow.** The crash fires *exactly* when `area` flips to 99 — i.e. the
  instant popos spawn and `mhfu_tick` runs its **heavy branch** (`string.format`
  per popo + `table.concat`). The lua_host poll thread is created with only an
  **8 KB stack** (`sceKernelCreateThread(..., 0x2000, ...)` in `lua_host_init`).
  Lua `lua_pcall` + `string.format` + the C bindings on an 8 KB PSP thread stack
  is a classic overflow → corruption → clean exit. The log shows the *previous*
  tick (`popos=0`) because the crashing tick dies mid-format before it can log.
  **Fix to try:** bump the create-thread stack to `0x10000` (64 KB) — one-liner —
  and re-run the cold-boot walker. (Note: the `scr==17` read-gate already applied
  does NOT cover this — in section 1 `scr==17` AND popos present, so the heavy
  branch still runs; the stack is the suspect, not the gate.)
* Secondary hypothesis: the 2 Hz poll thread reading game RAM *during the
  section-1 load window* tips the engine into the exit. The framework's own
  `mhfu_spawn_poll` walks the registry too but historically worked in section 1
  (popo_growth), so the delta is the Lua VM thread (stack/timing). Less likely
  than the stack overflow given the crash timing lines up with first popo spawn.
* **Mitigation applied (untested against the crash):** the demo `mhfu_tick` now
  **gates ALL reads behind `scr==17`** (a stable in-area frame) and only reads
  the single fixed screen-state byte otherwise — so the poll thread touches no
  registry/area memory during a load (`scr` 1/2). This *may* let it survive to
  the stable section-1 frame and finally log popo HP; **needs a verification run
  next session.**

To demonstrate live HP next session: boot with the **gated lua_host-only** PRX,
reach section 1, and check the log — OR, if the crash persists, bisect by
disabling the lua_host poll thread's reads entirely on the first ~3 s after an
`area` change, and/or move the popo read off the poll thread onto the framework's
existing `mhfu_on_monster_spawned` event (which fires at a known-safe point).

**Test artifacts:** `tools/smoke_lua_host.py` (cold-boot + state recipe — note
the `--state` double-launch caveat found here), reusable via `auto_cold_boot.py`.

**Env note:** the installed PRX at `~/.config/ppsspp/.../mhfu_framework.prx` is
now a **combined lua_host + tigrex_spin** build; the pre-Lua original is backed
up alongside as `mhfu_framework.prx.bak-pre-lua-*`.

**Next — Phase 2:** bind the AI override events (`on_action_input` first) and
port `tigrex_spin` to a Lua script as the acceptance test.

## Sources

PSP Lua: Wikibooks PSP Development/Lua; pspdev/psp-packages (`lua51/53/54`);
pspdev/psp-ports lua; PSP-Archive/Big-LuaPlayer; ONELua. LuaJIT-no:
libcg/LuaJIT (port broken), luajit.org/status. Size/config: Lua LTN-002;
lua-l ARM Cortex-M4 thread; eLua LTR docs; lua.org/source/5.4/luaconf.h.
PRX/PSP mechanics: forums.ps2dev.org PRX build + heap; PPSSPP Allegrex CPU
docs; Aquaria-on-PSP (Wolfire) custom Lua allocator. Sandboxing: Luau sandbox;
lua-users GC-in-realtime-games. Alternatives: Squirrel (ps2dev forum, Wikipedia),
AngelScript (gamedev.net PSP), Berry/Wren/PocketPy repos, schemescape
"smallest scripting language" benchmark.
```

---

## Phase 2 RESULTS — BUILT 2026-06-02, awaiting HITL verification

**Deliverable: a PRX that loads a Lua mod replicating `tigrex_spin`.** Installed
at `~/.config/ppsspp/PSP/PLUGINS/mhfu_framework/mhfu_framework.prx`
(pre-Phase-2 build backed up as `mhfu_framework.prx.bak-pre-luaspin-*`).
Manifest: `lua_host` ENABLED, C `tigrex_spin` DISABLED (both claim the vt[8]
`action_decided` chain → would CONFLICT; the Lua port supersedes the C mod).

**Lua-facing AI override API** (`framework/prx/mods/lua_host/mod.cpp`):

| Lua call | C binding | Semantics |
|----------|-----------|-----------|
| `mhfu.on_quest_targets_building(fn)` | `MHFU_EVENT_QUEST_TARGETS_BUILDING` | `fn(quest)` — edit list before model-load |
| `mhfu.on_bigmonster_spawn(fn)` | `mhfu_on_bigmonster_spawn` | `fn(ent,type,slot,hp)` (poll thread) |
| `mhfu.on_bigmonster_death(fn)` | `mhfu_on_bigmonster_death` | `fn(ent,type,slot)` (poll thread) |
| `mhfu.on_ai_overlay_loaded(fn)` | `mhfu_on_ai_overlay_loaded` | `fn()` observe (load-bearing) |
| `mhfu.on_bigmonster_slot_picked(fn,prio)` | `mhfu_on_bigmonster_slot_picked` | `fn(ctx,slot)->slot` (OOB-clamped) |
| `mhfu.on_bigmonster_action_input(fn,prio)` | `mhfu_on_bigmonster_action_input` | `fn(ctx,input)->input` (force path) |
| `mhfu.on_bigmonster_action_decided(fn,prio)` | `mhfu_on_bigmonster_action_decided` | `fn(ctx,value)->value` (vt[8] post) |

ctx table = `{ entity, type, slot, input }` (+ `count` for slot_picked). Plus
helper bindings `write_u8/u16/u32`, `entity_set_size`, `quest_has`,
`quest_replace_monster`, `action_ptr_for`, and the Phase-1 read/getter set.

**Design notes:**
- **Return-value threading**: each override C trampoline pushes the ctx + the
  engine's current value, `lua_pcall(…,1 result)`, and casts the Lua return
  back into the engine register (uint32 ptr / uint16 input / uint8 slot). Lua
  is built `LUA_32BITS` (lua_Integer = int32), so tigrex outcome pointers
  (0x08–0x0A range, < 2^31) round-trip exactly as positive integers.
- **Concurrency**: the override callbacks run on the GAME thread inside the
  engine call stack; spawn/death on the framework 5 Hz poll thread; the
  optional `mhfu_tick()` on our 2 Hz worker. The non-reentrant VM is serialised
  by a binary semaphore (`lua_enter`/`lua_leave`); `g_ready` gates trampolines
  until setup + refs are built; the panic handler releases the lock before
  parking so a dead VM can't hang the game thread.
- **Script packaging**: `mods/lua_host/scripts/tigrex_spin.lua` is the source of
  truth; `tools/embed_lua.py` emits a committed `*.lua.h` C string literal
  (octal escapes — no signed-char narrowing on UTF-8) that lua_host `#include`s
  and runs. `make -f Makefile.psp embed` (HOST) regenerates after a `.lua` edit;
  the Docker build needs no python. This is the bridge until Phase 3 ships
  memstick `.lua` loading.

**HITL acceptance test** (same env as the C mod — the Giadrome quest, NOT the
snow popo section-1): cold-boot PPSSPP (plugins load only on cold boot; `--state`
bypasses them), start the Giadrome quest. Expected `framework.log`:
`[lua_host] VM ready …`, `[tigrex_spin.lua] registered`, then on quest begin
`[tigrex_spin.lua] giadrome -> tigrex (native coords)`, on spawn
`… tigrex spawn … (resized 0.5x)`, then on the first natural spin
`… SPIN slot N cached …` ×3 and `… natural SPIN observed -> pin armed …`,
after which the 0.5x Tigrex stays locked spinning until first hit
(`… hp X -> Y, releasing pin`). PASS = behaviour matches the C `tigrex_spin`.

**Risk flagged for HITL**: running a `lua_pcall` per vt[8] decision on the
333 MHz game thread is heavier than the C mod's inline check. Steady-state hot
path does only integer ops + 12 `write_*` C calls (no string.format), so cost
should be bounded, but if it stutters/freezes the mitigations are: gate the
override on `pin_active` earlier, or move the maintenance writes into a single
C helper binding. Report observed behaviour to decide.
