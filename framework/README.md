# MHFU Modding Framework

A two-headed modding framework for **Monster Hunter Freedom Unite** (MHFU)
running on PPSSPP. Mods author against named events
(`mhfu_on_quest_beginning`, `mhfu_on_quest_entered`, …) and the framework
takes care of everything that sits between an event name and the MIPS
instruction that triggers it.

Two backends share **one event API**:

| Backend                 | Where it runs                | When to use                                  |
|-------------------------|------------------------------|----------------------------------------------|
| **Live (Python)**       | Outside PPSSPP, over the WS debugger | Iterate, debug, RE new events, hot-reload  |
| **PRX (C++, compiled)** | Inside the emulated PSP, via PPSSPP's plugin loader | Ship to other players, native game speed |
| **Lua (in-PRX script)** | Inside the emulated PSP, run by the `lua_host` PRX mod | Ship native-speed mods without recompiling; hot-reload `.lua` off the memstick |

Both compiled backends expose the same event names; a mod proven out in
live mode can be ported to a PRX with minimal changes. The **Lua**
platform (`prx/mods/lua_host/`) runs sandboxed `.lua` scripts off the
memstick against the same events through a curated `mhfu.*` API — author
without a toolchain, edit live. See `prx/mods/lua_host/README.md`.

## Layout

```
framework/
├── README.md                  this file
└── prx/
    ├── README.md              PRX SDK: layout, authoring, arbitration, gotchas
    ├── include/mhfu/          public SDK (events, memory, entity, hooks, mod, mips)
    ├── src/core/              C++ core: registry, trampoline, hookmgr, modtable, bootstrap
    ├── mods/<id>/mod.cpp      one descriptor mod each (MHFU_MOD(...))
    ├── mods/experimental/     research hooks (off by default)
    ├── build/mods.manifest    which mods compose into the single PRX
    ├── Makefile / Makefile.psp wrapper + PSP build (pspdev Docker)
    └── framework_exports.exp  symbols a future separate mod PRX imports

src/mhfu_bot/                   (sibling) — Python live-framework runtime
├── events/                    high-level mod-facing event API
├── hooks/                     low-level hook registry + code cave
└── mips/                      MIPS R4000 / Allegrex encoder
```

The Python live framework lives in `src/mhfu_bot/`, alongside the rest of
the project's runtime tooling (debugger client, savestate registry,
session lifecycle).

## How a mod is structured

### Live (Python)

```python
from mhfu_bot.events import mhfu_on_quest_beginning, mhfu_on_quest_entered

async def hello_beginning(ctx):
    print(f"Quest starting — cell value will become {ctx.regs.a1}")

async def hello_entered(ctx):
    area = await ctx.client.read_u16(0x08B0C7DC)
    print(f"Quest entered — area_index = {area}")

def register():
    mhfu_on_quest_beginning(hello_beginning)
    mhfu_on_quest_entered(hello_entered)
```

That's it. Run via `scripts/run_hello_world_mod.py` to see it fire.

### PRX (C++)

A mod is one descriptor TU; the framework collects it from the mod table
and calls `init()`:

```c
#include "mhfu/mhfu.h"

static void on_beginning(const mhfu_event_ctx_t *ctx) {
    mhfu_log("Quest starting — timer becoming %u", ctx->cell_value);
}
static void on_entered(const mhfu_event_ctx_t *ctx) {
    (void)ctx;
    mhfu_log("Quest entered — area_index = %u", mhfu_get_area_index());
}

static int my_init(void) {
    mhfu_on_quest_beginning(on_beginning);
    mhfu_on_quest_entered(on_entered);
    return 0;
}

MHFU_MOD(.id = "my_mod", .version = "1.0",
         .needs = 0, .conflicts = 0, .init = my_init, .shutdown = 0);
```

Add `my_mod` to `prx/build/mods.manifest`, `make` — it composes into the
single `mhfu_framework.prx`. See `prx/README.md` for hook arbitration
(events vs exclusive patches) and the JIT-bypass install pattern.

## Status

| Phase | Component | Status |
|-------|-----------|--------|
| 1 | MIPS R4000 encoder + golden-vector tests | ✅ done |
| 2 | Hook registry + code-cave allocator | ✅ done |
| 3 | Identify `quest_beginning` / `quest_entered` anchors via PPSSPP+Ghidra | ✅ done (writer PCs pinned) |
| 4 | High-level event API + dispatcher (live mode) | ✅ done |
| 5 | Hello-World sample mod (live mode) | ✅ verified end-to-end |
| 6 | PRX framework SDK + trampoline installer | ✅ verified end-to-end (Section 17.3) |
| 7 | Section-traversal + spawn events (POLL trigger) | ✅ done (Section 17.4) |
| 8 | First runtime mod: popo_growth | ✅ visually verified in-game (Section 17.4) |
| 9 | Quest monster-injection mod (Tigrex into Giadrome) | ✅ verified in-game (Section 31) |
| 10 | **Framework refactor → C++ core + hookmgr + descriptor mods + typed SDK** | ✅ builds + in-PPSSPP runtime re-verified (2026-05-30; Tigrex injection on snow Giadrome quest) |
| 11 | Region detection (NA/JP) + approach-B drop-in loader | 🟥 stubbed / spike pending |
| 12 | Documentation | ✅ done |
| 13 | **Lua scripting platform** (sandboxed Lua 5.4 in the PRX, memstick scripts, hot reload, OO API) | ✅ Phases 0–4 verified live (`prx/mods/lua_host/`) |
| 14 | AI override-event API (force big-monster actions coherently) | ✅ verified (Tigrex spin-lock; `docs/AI_SCRIPTING_ENGINE.md` §32k) |
| 15 | Multi-big-monster: N same-family (target-group split + entity clone) | ✅ verified (2× then 10× Tigrex; `docs/BIG_MONSTER_OVERLAY_RELOCATION.md`, CLAUDE.md §48–49) |
| 16 | HUD/overlay: projected monster nameplates | ✅ shipped (`prx/mods/monster_nameplates/`) |
| 17 | Free camera (quest orbit + village) | ✅ shipped (`prx/mods/lua_host/scripts/freecam.lua`, `docs/agent_camera_freecam.md`) |

The PRX path is proven end-to-end: trampolines patch correctly, the
spawn-poll thread tracks new entities, and mods (popo_growth size
oscillator; tigrex_inject quest injection) drive the engine — visually
confirmed in real gameplay.

Architecture (2026-05-29): the old monolithic `framework.c` with
`#if MHFU_EMBED_*` blocks was replaced by a C++ core (`prx/src/core/`),
a public SDK (`prx/include/mhfu/`), a **hookmgr** arbitration layer, and
self-contained descriptor mods (`prx/mods/<id>/mod.cpp`). Because PPSSPP
wedges with two plugin PRXes co-loaded, mods are **statically composed**
into the single `mhfu_framework.prx` ("approach A") via
`prx/build/mods.manifest`. True drop-in loading ("approach B" — the
framework `LoadModule`s mod PRXes itself) is a pending spike.

## Where to look next

- **Add a new event**: edit `src/mhfu_bot/events/addresses.py`, then
  mirror the entry into `framework/prx/include/mhfu/addresses.h`, add an
  `MHFU_EVENT_*` id in `include/mhfu/events.h` + a dispatcher in
  `src/core/registry.cpp` (and its trampoline in `src/core/trampoline.cpp`).
- **Add an event filter**: the `_is_event_fire` predicate in
  `src/mhfu_bot/events/dispatcher.py` decides whether a memory-BP fire
  actually corresponds to the event. Tighten it per event.
- **Discover a new event's anchor PC**: write a savestate that lets you
  trigger the event with a known input; run `scripts/re_quest_events.py`
  (or a copy with new watch cells); read `docs/quest_event_re.json`.
- **Move the code cave**: re-run `scripts/find_code_cave.py` against
  whatever in-game state you care about; pick a sufficiently large
  zero-filled region; update `DEFAULT_CAVE_BASE` in
  `src/mhfu_bot/hooks/code_cave.py`.

## License

MIT. Same as the surrounding project.
