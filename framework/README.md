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
| **PRX (C, compiled)**   | Inside the emulated PSP, via PPSSPP's plugin loader | Ship to other players, native game speed |

Both expose the same event names; a mod proven out in live mode can be
ported to a PRX with minimal changes.

## Layout

```
framework/
├── README.md                  this file
└── prx/
    ├── README.md              PRX-specific notes
    ├── include/
    │   ├── mhfu_framework.h           public mod API
    │   └── mhfu_framework_addresses.h per-region address table
    ├── src/
    │   └── framework.c        the framework PRX itself
    ├── Makefile               wrapper that drives pspdev Docker
    ├── Makefile.psp           the actual PSP build
    ├── framework_exports.exp  symbols downstream mods import
    └── mods/
        └── hello_world_prx/   sample mod (mirrors mods/hello_world/)

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

### PRX (C)

```c
#include "mhfu_framework.h"

static void on_beginning(const mhfu_event_ctx_t *ctx) {
    mhfu_log("Quest starting — timer becoming %u", ctx->cell_value);
}

static void on_entered(const mhfu_event_ctx_t *ctx) {
    mhfu_log("Quest entered — area_index = %u", mhfu_get_area_index());
}

int module_start(SceSize args, void *argp) {
    mhfu_on_quest_beginning(on_beginning);
    mhfu_on_quest_entered(on_entered);
    return 0;
}
```

Compiles to `hello_world.prx` via the pspdev Docker image; user drops the
folder into `<memstick>/PSP/PLUGINS/`.

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
| 9 | Standalone mod PRXes (linked vs framework exports) | 🟡 builds + links cleanly; co-load with framework wedges MHFU at boot — workaround `MHFU_EMBED_POPO_GROWTH 1` |
| 10 | Region detection (NA/JP) + on-disk mod enumeration | 🟥 stubbed |
| 11 | Documentation | ✅ done |

The PRX path is proven end-to-end: trampolines patch correctly, the
spawn-poll thread tracks new entities, and the embedded popo_growth
mod oscillates Popo `size_scale` 0.35× ↔ 2.0× over a 5 s cycle in
snowy-mountains section 1 — visually confirmed in real gameplay
2026-05-25.

Known open issue: PPSSPP wedges MHFU at boot when two plugin PRXes
are co-loaded (verified with a degenerate no-op mod), so the working
binary today inlines mod logic into the framework PRX behind the
`MHFU_EMBED_POPO_GROWTH 1` compile-time flag. The standalone
`framework/prx/mods/popo_growth_prx/` source is the canonical mod
example for when the co-load issue is resolved.

## Where to look next

- **Add a new event**: edit `src/mhfu_bot/events/addresses.py`, then
  mirror the entry into `framework/prx/include/mhfu_framework_addresses.h`
  and add an `MHFU_EVENT_*` enum + dispatcher in `framework.c`.
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
