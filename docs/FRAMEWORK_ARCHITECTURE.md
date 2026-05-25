# MHFU Modding Framework — Architecture

This document is the engineering reference. For a user-facing intro, see
`framework/README.md`. For the chronological session log of how this came
to be, see `docs/agent_session_log.md`.

## Goal

Let mod authors hook into MHFU's in-game events (quest start, area
load, monster spawn, player death, …) and react in real Python or
real C, without each mod author having to redo the reverse-engineering
work for the writer instructions inside the game binary.

The framework owns the **event → MIPS address** mapping. Mod authors
own the **event → behavior** mapping.

## Two backends, one surface

The mod-author-facing API is the same in both backends:

```
mhfu_on_<event>(callback)
```

What differs is *how* the callback fires.

### Live backend (Python)

```
┌────────────┐         WebSocket           ┌────────────┐
│   Mod.py   │  ◀─ HookFireContext ─◀──┐  │  PPSSPP    │
│            │                          │  │            │
│  register()│                          │  │  CPU       │
│   ↓        │                          │  │  ┃         │
│  on_event()│                          │  │  ▼ writes  │
└──────┬─────┘                          │  │  cell      │
       │                                │  │            │
       ▼                                │  │  PPSSPP    │
┌──────────────────────────────┐        │  │  memory.bp │
│  EventDispatcher             │◀───────┘  │  fires:    │
│  (mhfu_bot/events/dispatcher)│           │  cpu       │
│                              │   adds    │  halt      │
│  Memory write BP on cell ──▶ │   memory  │            │
│  on_step handler reads       │   BP    ▶ │            │
│  regs+cell, dispatches to    │           │            │
│  all subscribed callbacks    │           │            │
└──────────────────────────────┘           └────────────┘
```

* Trigger: PPSSPP's memory write BP with `change=True` and `log=True`.
* Dispatch: `EventDispatcher._handle_step()` runs as a background task
  (NOT inside the WS recv loop — that would deadlock on awaited RPCs).
* Per-event filter: `_is_event_fire()` returns `False` for spurious
  writes (e.g. area_index transitions 0→3 are not "entered"; only
  →98 is).
* Auto-resume: after dispatch, `client.resume()` is scheduled so PPSSPP
  keeps running.

Strengths: zero MIPS patching, easy hot-iterate, mod is plain Python.
Limits: dispatch latency ≈ multi-frame because PPSSPP is paused per
event; only suitable for rare events (quest start, area transitions),
not per-frame hot paths.

### PRX backend (C, compiled)

```
┌─────────────────┐        ┌──────────────────────┐
│ mod.prx         │        │ mhfu_framework.prx   │
│                 │        │                      │
│ module_start    │ ─call─▶│ mhfu_register_event() │
│   mhfu_on_*(cb) │        │   (appends to list)  │
└─────────────────┘        └──────┬───────────────┘
                                  │
                                  ▼
              ┌──────────────────────────────────────┐
              │ install_trampolines() [TODO]         │
              │ For each event anchor PC:            │
              │   ◦ build wrapper in .cave section   │
              │   ◦ patch anchor with J cave; NOP    │
              │   ◦ sceKernelIcacheInvalidateRange   │
              └──────────────┬───────────────────────┘
                             │
                  game runs … │ … reaches anchor PC
                             │
                             ▼
              ┌──────────────────────────────────────┐
              │ Wrapper in cave                      │
              │   save $a0..$a3, $ra                 │
              │   JAL mhfu_dispatch_quest_beginning  │
              │   restore                            │
              │   run displaced insn0, insn1         │
              │   J anchor+8; NOP                    │
              └──────────────┬───────────────────────┘
                             │
                             ▼
              ┌──────────────────────────────────────┐
              │ mhfu_dispatch_quest_beginning(...)   │
              │   walk callback list                 │
              │   invoke each with ctx               │
              └──────────────────────────────────────┘
```

Same registration surface; per-event struct contexts (`mhfu_event_ctx_t`
for quest events, `mhfu_map_section_ctx_t` for section traversal,
`mhfu_monster_spawn_ctx_t` for spawns). The dispatch happens inside the
emulated PSP CPU, at native speed. No WS round-trip, no breakpoint pause.

### Trigger modes (added Section 17.4)

Some writes PPSSPP's `memory.breakpoint.add` doesn't catch — the
sv.q-driven `area_index` 98 → arbitrary-section transition and the
entity-registry slot pointer writes are both invisible to mem-BPs
under `change=True` AND `change=False`. To keep the same mod-facing
event API regardless of trigger reliability, the dispatcher carries
TWO modes (see `events.addresses.EventMode`):

* `MEM_BP_WRITE` — memory write BP on the event's cell. Used by
  `mhfu_on_quest_beginning` (quest_timer) and `mhfu_on_quest_entered`
  (area_index → 98).
* `POLL` — async loop at the event's `poll_interval_s` reading the
  watched cells. Used by `mhfu_on_map_section_entered` (area_index)
  and `mhfu_on_monster_spawned` (entity registry slots 1..20).

The PRX backend resolves the same events differently — for
`area_index` it leans on the per-frame-executing writer PC
`0x0884CDFC` and does change-detection inside the wrapper (one
trampoline fires BOTH `quest_entered` and `map_section_entered`); for
monster spawns it runs a user-mode poll thread at 5 Hz.

## The shared building blocks

### `src/mhfu_bot/mips/`

MIPS R4000 / Allegrex instruction encoders.

* `encoder.py` — one function per opcode we need: `jal`, `j`, `nop`,
  `lui`, `ori`, `addiu`, `lw`, `sw`, `jr`, `move`, `beq`, `bne`, plus
  `li32` (two-instruction 32-bit constant load).
* `trampoline.py` — higher-level templates: `build_call_trampoline`,
  `build_prefix_postfix_wrapper`, `long_jump`. These are what the
  hook installer composes into a real trampoline.
* `tests.py` — golden-vector tests for every encoder. Run with
  `python -m mhfu_bot.mips.tests`. All 14 currently pass.

Critical facts:

* MIPS R4000 J/JAL encode a 28-bit absolute address (26-bit immediate
  ≪ 2). All MHFU user RAM lives in one 256 MiB region, so any
  intra-game J fits in a single 4-byte instruction. We don't need
  the `lui+ori+jr` long-jump form except for cross-region targets
  (scratchpad, kernel) which we never hit.
* **Branch delay slot**: every J/JAL/branch executes the next
  instruction before the jump takes effect. Trampoline templates
  always emit the delay slot explicitly. Getting this wrong is the
  classic MIPS-hook footgun.

### `src/mhfu_bot/hooks/`

Hook installation primitives (currently the PRX path's design,
re-targetable for live mode if we ever want trampoline-backed live
hooks).

* `code_cave.py` — bump allocator over a zero-filled region of user
  RAM. Default `0x08AE0000` (verified empty in MHFU EU at the
  quest-ready savestate — see `scripts/find_code_cave.py`).
  Slots are fixed-size (20 bytes = 5 instructions). Cave is verified
  empty before first use; raises if anything's already there.
* `registry.py` — `Hook` (one per patch site) + `HookRegistry`
  (process-global). Multiple callbacks can subscribe to a single
  Hook; `fire()` walks the list with per-callback exception
  isolation.
* `install.py` — `install_hook(client, registry, cave, target_addr)`:
  reads displaced bytes, allocates a slot, writes the trampoline,
  arms a BP at slot start, patches the call site.

### `src/mhfu_bot/events/`

Mod-facing high-level API.

* `addresses.py` — the single source of truth for which PC each
  named event hooks. EU only today; multi-region structure is in
  place (just needs population).
* `dispatcher.py` — the live-mode implementation. Carries both
  trigger backends: memory-BP halts with per-event predicate filter
  + background-task dispatch (for `mhfu_on_quest_*`); async POLL
  loops with per-event change attribution (for
  `mhfu_on_map_section_entered` + `mhfu_on_monster_spawned`). Builds
  per-event struct contexts (`MapSectionContext`,
  `MonsterSpawnContext`) so mod authors get named fields.
* `api.py` — module-level convenience functions
  (`mhfu_on_quest_beginning`, `mhfu_on_quest_entered`,
  `mhfu_on_map_section_entered`, `mhfu_on_monster_spawned`,
  `install_all_events`) and the process-global dispatcher singleton.

## Discovery workflow

How a new event becomes a framework primitive:

1. **Make a savestate** that lets you trigger the event with a single
   known input.
2. **Identify candidate cells** that change at the event moment. Often
   you already know them (HP, timer, area_index, etc.); if not, use
   the existing diff-sampling methodology in `docs/agent_session_log.md`.
3. **Run `scripts/re_quest_events.py`** (or a fork with your cells) to
   capture writer PCs via memory write BPs + `cpu.stepping` events.
   The script auto-resumes PPSSPP after each halt and persists hits
   to `docs/quest_event_re.json`.
4. **Pick a PC**:
   * Prefer EBOOT-resident PCs (range `0x0884xxxx..0x0887xxxx`)
     over overlay-loaded PCs (range `0x09Axxxxx`) — overlay
     addresses can drift across runs.
   * If multiple PCs fire near the same event, pick the one whose
     `change=True` write best matches the event semantics.
5. **Add to `addresses.py`** (Python side) and
   `mhfu_framework_addresses.h` (C side). Keep them in lock-step.
6. **Update the per-event predicate** in `dispatcher.py::_is_event_fire`
   if needed (filters out spurious writes).
7. **Add a `mhfu_on_<event>` convenience function** in `api.py` and
   an enum entry + dispatcher in `framework/prx/src/framework.c`.
8. **Smoke test**: add a `print` callback in `mods/hello_world/mod.py`
   and re-run `scripts/run_hello_world_mod.py`.

## Why memory BPs and not execution BPs (live mode)

Initial design was to set execution BPs at the writer PC. That
turned out to fire every frame — the writer instruction is reached
during normal play but only **changes the cell** at quest start. So
the BP fires constantly, PPSSPP halts every frame, input dies.

Memory BPs with `change=True` solve this cleanly: PPSSPP only halts
when the cell value actually transitions. Same fire-on-event semantic
as the user wanted; no per-frame pause storm.

## Why background-task dispatch (live mode)

The PPSSPP WS client's recv loop awaits the stepping handler. If the
handler awaits an RPC (like `cpu.getAllRegs`), the RPC's response
arrives on the WS, but the recv loop is blocked inside the handler
and can't deliver it. Deadlock.

Fix: handler schedules the real work via `asyncio.create_task` and
returns immediately. Recv loop continues processing messages,
including responses to the in-flight RPC. The task gets its response,
finishes dispatch, and schedules `cpu.resume`.

## Known caveats

* **Code cave location is region-specific.** `0x08AE0000` is verified
  empty in MHFU EU at the quest-ready savestate. If you load a state
  with overlays in that region, the cave verification will raise.
  Run `scripts/find_code_cave.py` against your scenario to pick a
  different base.
* **Overlay-loaded PCs are not stable hook targets.** Functions in
  the high RAM range (`0x09Axxxxx`) move between sessions. Always
  prefer EBOOT-range anchors.
* **One quest-start triggers `mhfu_on_quest_beginning` twice in our
  current setup.** quest_timer transitions at least twice during the
  setup chain. Add a dedup-by-state filter if your mod needs strict
  once-per-quest semantics.
* **PRX trampoline installer is implemented** (2026-05-25) in
  `framework/prx/src/framework.c::install_trampoline_for()`. Builds
  wrappers in a 64-byte-aligned BSS code cave using the in-PRX MIPS
  encoder (`src/mips_encoder.h`), patches anchors with `j cave; nop`,
  flushes dcache + icache via `sceKernelDcacheWritebackInvalidateAll`
  / `sceKernelIcacheInvalidateAll`. Mirrors the Python-side hook
  installer one-for-one; same wrapper shape. **End-to-end verified
  2026-05-25 Section 17.4** — first runtime mod (popo_growth)
  visually confirmed in real gameplay.

* **PPSSPP mem-BPs miss some write paths** (Section 17.4). The
  sv.q-driven `area_index` 98 → 99 transition and the entity-
  registry slot pointer writes don't fire `cpu.stepping` events with
  reason `memory.breakpoint` under any combination of
  `change=True`/`False`. Live framework events for those targets use
  the POLL trigger mode instead. If you discover an event that
  reproduces this behavior, default to POLL — diagnosing PPSSPP's
  mem-BP coverage is not a productive yak.

* **PPSSPP wedges MHFU at boot with two plugin PRXes co-loaded**
  (Section 17.4). Framework alone reaches the main menu in ~12 s;
  framework + any second plugin (even a no-op that just calls
  `mhfu_on_*`) keeps screen state at 0 forever. Workaround in source
  today is `MHFU_EMBED_POPO_GROWTH 1` in `framework/prx/src/framework.c`
  inlining mod logic into the framework PRX; canonical standalone
  mod source remains at `framework/prx/mods/popo_growth_prx/` for
  when the co-load issue is resolved.

* **`size_scale` mirrors must all be written.** The per-entity size
  scalar at `+0x024` has mirrors at `+0x220`, `+0x224`, `+0x228`,
  `+0x270` and the game's per-frame sync routine restores `+0x024`
  from one of them inside one frame (≤16 ms). Mods that want a
  visible size change must write all five cells.
