# AI / Cutscene Scripting Engine — RE Notes

**Session 32, 2026-05-30** — first pass at finding monster-AI / cutscene
scripting infrastructure. Live RE was driven against the new save state
`giadrome_intro_sec6_edge` (UI slot 5 / file `_4.ppst`) — player just
outside snow sec5→sec6 boundary in the giadrome urgent quest. Walking
straight forward crosses into sec6 and the engine plays a ~20s+
scripted intro sequence (camera locks on the giadrome, monster runs
through scripted poses + roar, player input suspended).

> **Caveat — INTERIM.** All findings below are pinned in the *cutscene*
> context. Whether the same scripting mechanism drives normal-fight AI
> is plausible but not yet verified. Items marked **OPEN** still need
> follow-up RE.

## TL;DR

* The engine has **two distinct scripting primitives**:
  1. **`TaskBase` + `TaskManager`** — the top-level engine-wide
     coroutine system (GameTask, FadeTask, …). Used for high-level
     program states. *Not* the per-monster AI driver.
  2. **A per-entity scripting-slot processor** — the function
     `z_un_088637C4` at EBOOT `0x088637C4` walks an array of "anim
     slots" on a given entity, with stride `0x40` and slot count at
     `entity+0x122 (u16)`. Each slot is a small FSM with timer +
     target floats + state byte + enable byte. This is the system the
     giadrome cutscene uses to puppeteer the monster.
* `screen_state @ 0x08A8CA48` low byte takes value **`0x06`** during
  scripted cutscenes. That byte is the cutscene-mode oracle (NEW —
  joins the existing `0x11`=in-area, `0x01`=menu/loading set).
* The cutscene fires NO new top-level task (TaskMgr `active[1]`/`[3]`
  stay NULL through the entire sequence). The driver writes the
  monster's per-entity slot array directly.
* All 4 monsters (Popo, Anteka, Tigrex, Giadrome) share **identical
  vtable[+0x0C..+0x2C]** entries — strong confirmation that the
  generic-AI-engine claim from Section 19 is correct.

---

## TaskBase / TaskManager

### Class layout (verified by decomp + live dump)

`TaskBase` (from `tools/mhfu_external/mhp2g-decomp/include/task_base.hpp`):

```cpp
struct TaskBase {
    typedef void (TaskBase::*mem_fn)();
    virtual ~TaskBase();
    virtual void update();
    virtual void load() = 0;

    mem_fn action;          // PTMF, 8 bytes
    u8 overlay_group;
    u8 load_status;
    s8 load_delay;
    u32 id;
    u32 next_id;
};
```

Per-instance offsets (live):

```
+0x00 vtable
+0x04 action mem_fn (lo: fn-ptr / vtbl-offset)
+0x08 action mem_fn (hi: this-adjust)
+0x0C overlay_group  (u8)
+0x0D load_status    (u8)
+0x0E load_delay     (s8)
+0x0F padding
+0x10 id             (u32)
+0x14 next_id        (u32)
```

### Vtable layout (Itanium-style)

```
vt[+0x00] = offset_to_top      (0)
vt[+0x04] = typeinfo            (NULL — RTTI suppressed)
vt[+0x08] = D0 dtor (non-deleting)
vt[+0x0C] = D1 dtor (deleting)
vt[+0x10] = update()
vt[+0x14] = load()
```

### TaskManager (EU)

```
TaskManager**          0x08A8B1E8
TaskManager* (live)    0x08B0C6A4
  +0x00 active[4]                (TaskBase*[4])
  +0x10..             slab[0x14000]   (in-place task storage)
```

Currently observed: `active[0]=GameTask` (vtable `0x089BAC6C`),
`active[2]=FadeTask` (vtable `0x089BAA94`). Slots `[1]` / `[3]` stay
NULL through the entire giadrome intro.

### Dispatch contract

`TaskBase::update()`:
```cpp
if (!is_overlay_loaded()) return;
if (action) (this->*action)();
```

GameTask / FadeTask both **override `update()` directly** (vtable
slot +0x10) — they don't use the PTMF path. Subclasses are free to
use *either* mechanism. So as a modder you have two seams:
1. Vtable swap on `vt[+0x10]` of a task instance.
2. Write a non-zero `action` PTMF + leave `update()` as the base.

### What this is *not*

TaskBase **does not** drive monster AI. ObjBase / monster entities
inherit from a different hierarchy (`ModelBase` → `ObjBase`, see
`include/obj_base.hpp`). The monster's per-tick advance is on
`ObjBase.vt[5] = vtable_0x14` — already pinned in Section 22.

---

## The per-entity scripting-slot processor

### Function

```
z_un_088637C4   @  0x088637C4    EBOOT
```

* Called via **JALR** (no static J/JAL callers in EBOOT — confirmed
  by full `0x08800000..0x08D00000` scan in
  `tools/disasm_cutscene_driver.py`).
* Argument: `a0` = pointer to an entity (or some scripted object).
* The fn does **not** appear in the first 12 slots of any monster
  vtable, including the entity's *primary* vtable. **OPEN:** must be
  in a secondary vtable (multiple inheritance, see
  `cpp-secondary-vtables` memory) or a different parent's vt.

### Slot processing loop (live disasm)

```text
prologue:
  s5 = a0                    ; save entity
  v1 = (a0+0x110)            ; sub-struct ptr (script data)
  v0 = (a0+0x122) u16        ; slot count
  v1 = (v1).+0x14C           ; deref another level
  s2 = v1 + 0x150            ; world-pos anchor
  if (v0 == 0) goto epilogue ; bail if no slots
  s1 = s5                    ; s1 = entity (will stride)
  s0 = s5                    ; s0 = entity (paired u16 walk)
  s4 = 0                     ; loop index

loop body @0x0886380C:
  (lots of fp arithmetic on s1+0x10..+0x3F)
  ...
  ; key writers:
  SWC1 f0, 0x10(s1)          ; @0x088639A4 — main phase-timer cell
  SWC1 f0, 0x40(s5)          ; transform translate row .x
  SWC1 f0, 0x44(s5)          ; transform translate row .y
  SWC1 f0, 0x48(s5)          ; transform translate row .z

loop tail @0x08863B30:
  v0  = (s5+0x122) u16       ; reload slot count
  s4 += 1
  s1 += 0x40                 ; advance to next slot
  if (s4 < v0) goto loop body
  s0 += 2                    ; parallel u16-stride array

epilogue: return s3
```

### Slot layout (stride 0x40, indexed by `s1`)

Slot[k] starts at `entity + k*0x40`. Used fields, from the loop disasm:

| Offset | Read by | Written by | Inferred role |
|--------|---------|------------|---------------|
| +0x10  | `LWC1 f12,0x10(s1)` and `LWC1 f0,0x10(s1)` | `SWC1 f0,0x10(s1)` @0x088639A4 / @0x088638D0 / @0x08863A30 / @0x088639C0 | **phase timer** (float, accumulates per tick) |
| +0x14  | `LWC1 f1,0x14(s1)` | — | **timer increment per tick** |
| +0x18  | `LWC1 f1,0x1C(s1)` (target?) | — | **upper-bound** for timer |
| +0x1C  | (target?) | — | **target value / endpoint** |
| +0x24  | `LWC1 f2,0x24(s1)` | — | secondary value (delta?) |
| +0x30  | `LW v0,0x30(s1)` then dec | `SW v0,0x30(s1)` | **iteration counter** (decrements toward 0) |
| +0x34  | `LWC1 f1,0x34(s1)` | `SWC1 f1,0x34(s1)` / `ADD.S` accumulate | **secondary timer/accumulator** |
| +0x3C  | `LHU v0,0x3C(s1)` then OR 1 | `SH v0,0x3C(s1)` | **status u16** — bit 0 flags "done", bit 1 flags "secondary" |
| +0x3E  | `LB v0,0x3E(s1)` | `SB zero,0x3E(s1)` | **state byte** (s8) |
| +0x3F  | — | `SB zero,0x3F(s1)` (loop top clears each pass) | **enable byte** (cleared on every tick) |

### Slot script-data path

* `entity+0x110` → some script-data sub-struct ptr.
* `(entity+0x110).+0x14C` → another struct ptr.
* `+ 0x150` of that = `s2` in the driver; `s2.+0x30 / +0x34 / +0x38`
  hold target world position floats (XYZ).
* The driver writes those floats to `entity+0x40/+0x44/+0x48` —
  i.e. **drives the entity's translation row directly** from
  scripted target coords.

### Slot count is transient

`entity+0x122 (u16)` was `0` when I sampled mid-cutscene via Python
poll, even though the loop ran at least 3 iterations earlier (writer
PC fired at slot index 2 = entity+0x80). The slot count is set up
per-tick or per-sub-phase by some upstream system and cleared/zeroed
quickly. **OPEN** — pin the writer of `+0x122` and `+0x110` to find
that upstream system.

### Verified write targets during giadrome intro

These are entity cells the cutscene driver (or peer code in the
same frame) writes:

* `+0x010..+0x018` — heading vec components (cell `+0x10` is the
  main phase-timer for the slot at `s1=entity`; matches earlier
  finding that `+0x10` is engine-OUTPUT, not motion input).
* `+0x040..+0x048` — translation row (writes from `s2.+0x30..+0x38`).
* `+0x080..+0x0CF` — slot[2] (the giadrome intro's main sub-phase
  slot — `+0x0090` phase timer = float ramp 0..~410 then resets,
  `+0x00BC` = sub-state u8 cycling 3/6/7/8, `+0x00C8` = anim
  fractional accumulator).
* `+0x01D4` — high-byte counter (0x101 → 0x201 → 0x301 across
  sub-phases — anim-step ID).
* `+0x01F4` — packed yaw u16 (the cutscene steers the giadrome's
  facing every sub-phase).
* `+0x0324` — PRNG seed lo (jumps `0x3E9 ↔ 0x3EA`).
* `+0x0334` — AI-state byte: `0 → 3`. The value `3` is a NEW state
  not in popo's observed set `{1,2,5,10}`. **OPEN** — meaning of
  state 3 (cutscene-puppeteer mode?).

---

## Generic-AI-engine cross-check

While probing, dumped first 12 vtable slots of:

```
giadrome 0x089BC950
popo     0x089BC560
anteka   0x089BC074
tigrex   0x089BB69C
```

Result — these slots are **identical** across all 4 species:

```
vt[+0x0C] = 0x09AC4D30
vt[+0x10] = 0x09AC47C0
vt[+0x14] = 0x08864634
vt[+0x1C] = 0x088646AC
vt[+0x20] = 0x08865254     (popo vt[8] — anim probability picker, prior RE)
vt[+0x24] = 0x0886574C
vt[+0x28] = 0x08865810
vt[+0x2C] = 0x09ADA4B0
```

Only `vt[+0x08]` and `vt[+0x18]` differ (each species' specific
overrides). Strongly reinforces CLAUDE.md `ai-engine-is-generic`:
build one editor / mod base for all monsters at once.

## Cutscene-mode oracle

```
0x08A8CA48 (u8)   screen_state
    0x11 = in-area, controllable
    0x01 = menu / loading
    0x06 = SCRIPTED CUTSCENE PLAYBACK   (NEW)
```

Sequence on sec5→sec6 cross: `0x11 → 0x01 (loading) → 0x06 (cutscene)`.
Did not return to `0x11` within my 18s window — cutscene >~14s.

## Evdemo (cutscene singleton — partial)

Decomp puts an `Evdemo` singleton with a trigger fn:
```cpp
struct Evdemo : Singleton<Evdemo> { /* 0xA0 bytes opaque */ };
bool func_eboot_088D0824(Evdemo *, u32 demo_id, u8);
```
JP singleton ptr cell is at `0x09A4ADAC` (overlay range). EU
equivalent **OPEN** — reads NULL at both `0x09A4ADAC` and
`0x09A4ADAC + 0x4F30` (the static-data delta). Likely shifted within
the overlay; needs a static scan of `0x09A40000..0x09A60000` for a
plausible 0xA0-byte block + back-resolve the ptr cell.

If we can find Evdemo + call `func_eboot_088D0824(ev, demo_id, 0)`
from a PRX, we can trigger custom scripted scenes by demo_id.

---

## Injection-point candidates for the modding framework

* **(A) Vtable swap on the entity-tick path** — already proven on
  popo vt[8] (Section 22). We can override how a monster ticks by
  swapping the entity's vtable[+0x10] (= primary `update` slot).
* **(B) Mem-BP on `entity+0x122`** at next session — when written,
  capture the writer PC. That gives us the **slot installer** — the
  function that activates scripted slots on an entity. With that PC
  + its args we can synthesise our own slot installations and run
  custom monster animations on demand from a mod.
* **(C) Re-use `z_un_088637C4` directly** — once we know slot
  fields fully, allocate a slot record in a known cell of the
  entity, set `+0x122` to slot count, write target floats to
  `entity+0x110→...→+0x150` and the engine ticks them for free.
* **(D) Evdemo route** — once EU Evdemo singleton is pinned, calling
  `func_eboot_088D0824(ev, our_id, 0)` triggers a cutscene by id.
  Existing demo_ids can be enumerated by static scan of
  `func_eboot_088D0824`'s switch table. Mod could ship its own
  demo-id handlers if we can extend the table.

---

## Tools added this session

| Tool | Purpose |
|------|---------|
| `tools/probe_task_system.py` | Live-dump TaskManager / FadeTask / Quest singletons (EU). One-shot recon. |
| `tools/probe_task_dump.py` | Live-dump TaskBase instances (vtable, action, fields). |
| `tools/probe_pre_intro.py` | Pre-intro snapshot: entity registry + scan static region for overlay ptrs. |
| `tools/re_event_trace.py` | **Generalised live event tracer.** CLI: state name + watches + analog inputs + duration. Outputs chronological diff stream per watch. Reusable for any "drive game into state X, see what changes" RE. |
| `tools/find_cutscene_writer.py` | Trigger cutscene + mem-BP on `+0x0090` to capture writer PC. |
| `tools/disasm_cutscene_driver.py` | Trigger cutscene + disasm `z_un_088637C4` + scan EBOOT for J/JAL callers + snapshot key cells. |
| `tools/dump_giadrome_slots.py` | Trigger cutscene + dump entity vtable / +0x110 / +0x122 / slot array + cross-species vtable comparison. |

`re_event_trace.py` is the keeper — future "drive + observe" tasks
should reuse it rather than write new ad-hoc snippets.

## Unification (Section 32b, 2026-05-30 continued)

The slot-installer hunt produced a **major reframe** of the
interpretation above. A code-BP at `z_un_088637C4` entry captured
`a0 = 0x090BD5B0 = giadrome + 0x80`, NOT the giadrome entity itself.
Re-mapping all offsets against `entity+0x80` as the base:

| Driver-arg offset | Equivalent entity offset | Existing CLAUDE.md role |
|---|---|---|
| `a0 + 0x110` | `entity + 0x190` | **action-list ptr** (read by applier `z_un_08863e70`, CLAUDE.md §19) |
| `a0 + 0x122` | `entity + 0x1A2` | **Action count (loop bound)** (already documented) |
| `a0 + 0x14C` deref | within action descriptor | descriptor-walk field |

So `z_un_088637C4` is **the per-frame action-list walker** of the
existing generic AI engine — *not a new system*. The "scripting
slots" I described above are the **action-descriptor slots** already
documented at `entity+0x1A0..+0x1FF` in CLAUDE.md `agent_memory_map.md`,
with stride `0x40` and count capped by `+0x1A2`.

Caller function = `z_un_08865044` (EBOOT). 0x6F0 bytes into that
function is the JALR to z_un_088637C4. The caller does big VFPU /
transform / quaternion work (it's a render-matrix pipeline), so this
is the SKELETON UPDATE call site — it invokes the action-list walker
to advance the entity's running animation each frame.

The "cutscene driver" I was hunting is just **the action-list walker
running on a custom action list installed at `entity+0x190`** during
the scripted intro.

### Where the installer lives

CLAUDE.md §19 traces the normal-AI install path:

  dispatcher `0x09AC5400` (overlay) reads `entity+0x324` seed →
  vt[8] = `0x08865254` (probability lookup over `entity+0x1AC`
  species table) → returns 32-bit anim ID → `z_un_08863e70`
  (applier) applies it via `z_un_0885f9a0`.

For the **cutscene**, the dispatcher path must be bypassed: something
writes `entity+0x190` directly with a custom action-list ptr (and
`entity+0x1A2` count). Candidate installer locations:
- A cutscene-specific overlay function that mirrors the dispatcher's
  output side but consumes a hardcoded action list (from quest data).
- The Evdemo `func_eboot_088D0824` trigger function (EU equivalent
  still unknown, but Section 32b shows its action would land here).

### Refined injection-point story for mods

To run a custom scripted animation on any monster:

1. Allocate a custom action-descriptor block in RAM (format = pairs
   of `(u16 action_id, u16 duration_ticks)` per CLAUDE.md §19e.2,
   plus the action data the picker indexes into).
2. Write its ptr into `entity+0x190` (= the action-list ptr).
3. Write the action count into `entity+0x1A2` (u16).
4. The engine's per-frame walker `z_un_088637C4` will tick through
   it for free.

This is the same surface used by the cutscene system — proven by
the giadrome intro live RE. Modders writing per-monster behaviour
just need to populate the action-descriptor block + flip those two
cells.

## Section 32d (2026-05-30) — Per-frame tick + override hook points

Driving the modding-framework override-event design (Q-set on
`on_bigmonster_action_decided`). Re-RE'd the full per-entity AI tick
chain end-to-end. (See §32g for why the originally-proposed
`anim_decided` event was dropped.)

### Per-entity AI tick `z_un_08865648` (real caller of walker)

Found by walking back from captured `ra=0x08865734` after a code-BP
at the walker entry. Six function prologues between `0x08865250` and
`0x08865800`; the one immediately before `ra` (= `0x08865648`) is
the real per-frame per-entity tick:

```
z_un_08865648(entity):       ; called per-frame for every entity
  v0 = z_un_08865c88(entity, 0x20000)
  if v0 != 0:    skip transform_builder + root-motion (paused?)
  else:
      z_un_088652dc(entity)                          ; transform_builder (Section 25)
      z_un_08863b70(entity+0x80, &locbuf, entity+0x10)   ; root-motion delta from anim
      entity+0x200 += locbuf  (VFPU VADD.T)          ; world-pos integrate
  for slot in 0..entity+0x1A2:
      if entity+(slot*0x40)+0xBE != 0:    continue   ; slot's direction byte already set
      if (entity+0x1A8+slot) u8 == 0:     continue   ; per-slot trigger flag off
      v0 = entity->vt[8](entity, entity+0x1B8+slot*2)   ; PROBE — non-zero = install
      if v0 == 0:                          continue
      v0 = entity->vt[8](entity, entity+0x1B8+slot*2)   ; PICK — returns u32 action_id
      z_un_0885f9a0(entity+0x190, v0, slot, 1)            ; INSTALL action_id into slot
  z_un_088637c4(entity+0x80)                          ; walker tick — advance slot timers/flags
```

### Two-layer ACTION → ANIM resolve

`z_un_0885f9a0` (installer, just 27 insns) does NOT write a slot
cell directly — it chains through an action→anim resolve:

```
z_un_0885f9a0(a0=action_list_ptr, a1=action_id, a2=slot, a3=1):
  anim_node_ptr = z_un_0885f928(action_list_ptr, action_id)
                  ; ACTION → ANIM resolve — walks the species anim graph by u16 ID
                  ; (per CLAUDE.md §19c.5: MSB-set IDs are no-op markers)
  if anim_node_ptr == 0: return    ; no anim found, slot left alone
  z_un_0885f848(action_list_ptr, anim_node_ptr, action_id, slot, t0=1)
                  ; APPLY — installs anim_node_ptr into slot[slot]+0x38, sets timers
```

### Cross-species vtable confirmation (live)

```
popo       vt @ 0x089BC560  vt[8] = 0x08865254
anteka     vt @ 0x089BC074  vt[8] = 0x08865254
tigrex     vt @ 0x089BB69C  vt[8] = 0x08865254
giadrome   vt @ 0x089BC950  vt[8] = 0x08865254     <-- live, vt ptr read from slot 1
```

All 4 species share vt[8]. **One hook covers every monster.** Cements
the generic-AI-engine claim from Section 19.

### Walker `z_un_088637C4` field map (refined)

Per-slot (stride 0x40, base entity+0x80, slot count at entity+0x1A2):

| Offset | Type | Role |
|--------|------|------|
| `+0x10` | f32  | anim time accumulator (advances by `f12` per call) |
| `+0x14` | f32  | anim time delta (ramp) |
| `+0x18` | f32  | anim phase target (post-blend time) |
| `+0x1C` | f32  | anim length / max time |
| `+0x24` | f32  | secondary ramp |
| `+0x30` | u32  | timer countdown (loop iter limit) |
| `+0x34` | f32  | secondary time accumulator |
| `+0x38` | u32  | anim_node_ptr (set by installer) |
| `+0x3C` | u16  | flags (bit0 = phase-1 done) |
| `+0x3D-0x3E` | s8 | direction byte (BE check at tick entry) |
| `+0x3F` | u8   | active flag (cleared at slot start) |

Parallel arrays on the entity:
- `entity+0x1A8 + slot` u8 — per-slot trigger flag (skip slot if 0).
- `entity+0x1B0 + slot*2` u16 — per-slot phase value 0..100 (normalised f).
- `entity+0x1B8 + slot*2` u16 — per-slot input passed to vt[8] (`$a1`).

Slot 0's root-motion delta is mirrored to `entity+0x40/0x44/0x48`
(the transform translation row).

### Override hook map for framework events

| Event | Hook site | Type | Layer |
|-------|-----------|------|-------|
| `on_bigmonster_action_decided` | vt[8] = `0x08865254` swap (Section 22 pattern) | sync override, $v0 mutable | behavioral pick |
| `on_bigmonster_ai_step`        | enter `z_un_08865648` (per-frame per-entity) | observe-only | tick |
| `on_bigmonster_damaged` (2026-06-30) | 5 Hz monster-poll HP-drop edge (reuses `g_last_hp[]`) | observe-only | poll |

`on_bigmonster_damaged` (lua `mhfu.on_bigmonster_damaged(ent,type,amount,hp,slot)`) fires
when a big monster TAKES damage — poll-derived (coalesces multi-hits per 200 ms, NOT
frame-accurate; zero new engine detours). Use it to REACT to being hit (re-assert a forced
action, trigger a custom move). For frame-accurate flinch SUPPRESSION intercept
`on_bigmonster_action` instead (the engine dispatches the hit-reaction through the executor).

The two action hooks are EBOOT-resident. Section 26 JIT bypass (install at
TITLE/MENU before first quest tick) applies to `0x08865648`. vt[8] swap
is JIT-immune (data lookup).

### Mod-side concept maps

- `outcome_ptr` (returned by vt[8]) = pointer into the per-species
  probability table at `entity+0x1AC`. Pointing at the chosen "action"
  record — a behavioral outcome with associated anim asset references
  embedded.
- `slot_state_ptr` (returned by `z_un_0885f928`) = the per-slot
  state-container node found by walking the action_list tree against
  the slot index. Where state lives, not what to play.

So **action_decided** lets mod replace WHAT the monster does. (We
considered an `anim_decided` hook on `z_un_0885f928` — see §32g below
for why it was dropped.)

## Section 32g (2026-05-30) — anim_decided collapses to action_decided

The framework initially proposed two override layers
(`action_decided` + `anim_decided`) on the theory that `z_un_0885f928`
transformed vt[8]'s output. RE'ing both functions live disproved that:

**vt[8] = `z_un_08865254` (34 insns)** returns the picked outcome:
- `0x3E8` sentinel ("no species table at +0x1AC")
- `0` ("no entry matched")
- else **`prob_table_base + offset` — a pointer INTO the species
  probability table** at `entity+0x1AC`. Smoke confirmed live values
  like `0x094270f8`, `0x094D69F4`, `0x09503F38` — overlay-range
  pointers into the species table data.

**z_un_0885f928 (30 insns)** is NOT a transform of vt[8]'s output. It
takes `(a0=action_list, a1=root_node_ptr (= action_list_ptr), a2=slot_idx)`
and walks the action_list tree searching for a node whose `+0x114` u16
== `slot_idx`. Returns that **slot's state-container node** (or 0).
Independent of vt[8].

Both values then flow into the applier `z_un_0885f848` separately:
- `a1` = slot_state_ptr (from z_un_0885f928)
- `a2` = outcome_ptr     (from vt[8])

So action_decided alone captures all behavioral override surface. The
former `anim_decided` would have hooked a structural state-container
lookup, which mods don't typically want.

Decision: **removed `mhfu_on_bigmonster_anim_decided` from the SDK**.
If a future need for hooking the slot-state lookup arises, it can be
re-added as `on_bigmonster_slot_state_resolved` (different event with
different semantics — not the same as the engine's anim resolve).

## Open follow-ups

> **Update (§32k, 2026-06-03):** the action-selection seam is SOLVED — see
> Section 32k. `entity+0x334` (#5) is the executor's "mode" byte (descr[6]-
> derived); the descriptor sub-record format (#3) is the 8-byte rows documented
> in §32k. Remaining big item: dump the descriptor table + force-cycle `a1` to
> enumerate/name each species' full moveset.

1. **Find the explicit installer call** that writes `entity+0x190`
   during cutscene init. Once we have its PC + arg signature we can
   call it directly from a PRX.
2. **Verify normal AI usage** — sample `entity+0x1A2` for popo
   during normal aggression. If non-zero, action-list walker is
   the same mechanism for combat AI.
3. **Slot field format** — re-RE the action-descriptor sub-records
   pointed at by the `+0x14C` deref. CLAUDE.md §19e.1 has partial
   info; reconcile with what `z_un_088637C4` reads (now mapped above).
4. **EU Evdemo singleton** — still deferred (3 approaches failed
   Section 32b). $gp-relative scan next time.
5. **Map `state byte = 3`** at `entity+0x334` — observe when else
   it appears; correlate with action ID.
6. **Disasm `z_un_0885f928`** (action→anim resolver) — confirm it has
   no third pick layer and document its anim-graph traversal.

# Section 32h — `ai_step` entry detour LIVE + big-monster action list RE (2026-05-30)

## ai_step wiring (task #35)

`mhfu_on_bigmonster_ai_step(cb, priority)` is now LIVE-installed via an
entry-detour on `z_un_08865648` (the per-frame per-entity AI tick).

### Mechanism

JAL-caller scan of EBOOT + overlay found only **2** call sites:

| Caller | Region | Context |
|--------|--------|---------|
| `0x09A74ECC` | OVL_A | per-entity tick chain (a0=entity from $s0) |
| `0x09AD3D6C` | OVL_B | transform/init path (reads +0x1F0/1F4/1F8 first) |

Both are in overlay (`0x09Axxxxx`), so we can't wrap a call-site behind
the TITLE/MENU gate (overlay isn't loaded then; `expect`-check fails).
Instead we patch the EBOOT entry of `z_un_08865648` itself.

Prologue (5 insns, only first 2 displaced):

```
0x08865648  ADDIU sp,sp,-0x20    = 0x27BDFFE0   <- replaced with J wrapper
0x0886564C  SW    ra,0xC(sp)     = 0xAFBF000C   <- replaced with NOP
0x08865650  SW    s2,0x8(sp)     = 0xAFB20008   (untouched — function resumes here)
0x08865654  SW    s1,0x4(sp)     = 0xAFB10004
0x08865658  MOVE  s2,a0          = 0x00809021
```

### Wrapper (12 insns, in code cave)

```
wrapper:
  addiu sp,sp,-0x20             ; our frame
  sw    ra,0x18(sp)             ; save caller ra
  sw    a0,0x10(sp)             ; save entity arg
  jal   mhfu_ai_step_dispatch_c ; helper(a0=entity)
  nop
  lw    a0,0x10(sp)             ; restore a0
  lw    ra,0x18(sp)             ; restore ra
  addiu sp,sp, 0x20             ; pop our frame
  addiu sp,sp,-0x20             ; replay displaced insn 1
  sw    ra,0xC(sp)              ; replay displaced insn 2
  j     0x08865650              ; resume function at entry+0x08
  nop                           ; J delay slot
```

Two deferred-quiet-patches land both words in the same poll iteration so
the JIT race window is microseconds.

### Live verification (cold boot, no PRX bypass)

```
[ai] action_decided installed on 4 species vt[8] slots
[ai] ai_step entry detour queued @ 0x08865648 (wrapper 0x09D87F50)
[hook] 'mhfu_ai' word @0x08865648 = 0x0a761fd4 (was 0x27bdffe0)  <- J 0x09D87F50
[install] quiet-patched 0x08865648 for 'mhfu_ai'
[hook] 'mhfu_ai' word @0x0886564c = 0x00000000 (was 0xafbf000c)  <- NOP
[install] quiet-patched 0x0886564c for 'mhfu_ai'
```

Wrapper disasm matches the spec exactly. `JAL` target lands on the
PRX-resident `mhfu_ai_step_dispatch_c` (`0x09D677F4` in this build).

## Big-monster action list RE (task #36)

### Tigrex (type 0x4B)

* `species_entry @ 0x09BC0FB0` (`0x09BB87C0 + 0x4B * 0x1D0`).
* **`+0x0C = 0x09BCF138` is NOT an action list** — it's hitzone data:
  16-byte (u16,u16) header `(0x0708, 0x2328)`, then a hitzone-material
  byte table (00/01/02/03 quadrants), then u32 numerics + float
  hitbox/range values.
* `entity+0x640` deref points at a per-entity scratch list with 8-byte
  records `(u8 idx, u8 0xFF marker, u32 zeros, u16 param_a, u16 param_b)`
  — looks like per-action damage/range params, not the action repertoire.
* Action selection for tigrex flows through vt[8] (`0x08865254`) reading
  the **probability table at `entity+0x1AC` = 0x09426760**, and vt[8]
  returns POINTERS into that table — not u16 action IDs.

Override mods on tigrex therefore return *pointer values* (e.g.
`0x094270F8`, `0x094D69F4` — observed engine picks in the giadrome smoke
log) rather than enum IDs. A u16 action-id enum doesn't apply cleanly,
so `mhfu/ai_actions.h` omits TIGREX_ACTION_*.

### Giadrome (type 0x4D)

* `species_entry @ 0x09BC1350` (`0x09BB87C0 + 0x4D * 0x1D0`).
* `+0x0C = 0x09D58570` (runtime-allocated in RAM heap; not present in
  giadrome_intro_sec6_edge but populated in tigrex_s6).
* Same `(u16 id, u16 duration_ticks)` record format as popo/anteka after
  an 8-byte header. **34 actions** — enumerated in `mhfu/ai_actions.h`
  as `GIADROME_ACTION_0x????`. Tigrex_s6 was the state where the list is
  fully resident.

Giadrome is classified "big" in the quest UI (counts as a quest target)
but its AI shape is the same as small monsters — fixed action list,
duration ticks, vt[8] returns u16 IDs not pointers.

### Global `0x09BD38F0`

Per CLAUDE.md §19g.3 this is the "big-monster runtime action list table".
First 11 records have format `(u8 action_id, u8 0xFF, u8 0, u8 sub_idx,
u16 0, u16 duration_ticks)` with action IDs `{0x00, 0x14, 0x16, 0x17,
0x18, 0x19, 0x1A, 0x1C, 0x1D, 0x1E, 0x1F}` and duration 236 ticks each.
After offset 0x58 the data switches to floats (per-action range/hitbox
params). This is the source for big-monster types whose `+0x640` is
runtime-swapped (type 0x3A per §19g.3 — not tigrex / not giadrome). Not
needed for the current SDK; revisit when a 0x3A monster is RE'd.

### `mhfu_action_is_valid` rewrite

Was a hand-maintained static array per species. Now walks the species
table at runtime:

```c
if (type == 0x4B) return 1;                          // tigrex — pointers
if (type not in {0x45, 0x46, 0x4D}) return 0;
list_ptr = *(u32 *)(0x09BB87C0 + type*0x1D0 + 0x0C);
walk (u16 id, u16 dur) records after 8-byte header until 0xFFFF/0.
```

Auto-updates when species data changes, removes a stale-table bug class.

# Section 32i — picker-vs-tick disambiguation + tigrex ptr cache (2026-05-30)

## Two hooks, two purposes

A common confusion: "isn't the picker the better hook than the per-frame
tick?" — yes, and we ALREADY hook the picker. There are two distinct
events with very different roles:

| Event | Hook target | Purpose |
|-------|-------------|---------|
| `action_decided` | **`vt[8] = 0x08865254`** (the picker) | OVERRIDE — return value flows through `0885f9a0` → `0885f848` |
| `ai_step` | entry of `z_un_08865648` (per-frame tick) | OBSERVE-ONLY — fires before the slot loop runs vt[8] internally |

The per-frame tick `z_un_08865648` is the FUNCTION inside which vt[8] gets
called per-slot. Hooking its entry gives observability ("a big monster is
about to tick"), but trying to override an action there would race with
the engine's own vt[8] call later in the same function.

`action_decided` is the right surface for "force a different action":
intercepts vt[8]'s return immediately before `0885f9a0` consumes it.

## Tigrex ptr resolution — passive cache

Tigrex's vt[8] returns a per-run pointer into entity+0x1AC's nested
sub-tables, not a stable u16 ID. To let mods address tigrex actions
symbolically:

* `tools/dump_tigrex_inputs.py` parses framework.log and emits
  `TIGREX_VT8_INPUT_0x????` macros for each observed vt8_input value
  (50 action inputs + 4 probe inputs from 1860 logged calls).
* The framework's `action_decided` dispatcher unconditionally snoops every
  `(vt8_input, engine_id)` pair into a per-species cache (96 entries each).
  Snooping happens even with zero subscribers, so the cache populates
  itself as soon as the framework boots into a quest.
* `mhfu_action_ptr_for(type, input)` returns the cached ptr — 0 if the
  engine hasn't picked that input yet this run.

Mod pattern:

```c
static uint32_t pin_to_charge(const mhfu_action_decision_ctx_t *c,
                              uint32_t engine_value)
{
    if (c->monster_type == MON_TIGREX) {
        uint32_t p = mhfu_action_ptr_for(MON_TIGREX, TIGREX_VT8_INPUT_0x04B1);
        if (p) return p;
    }
    return engine_value;
}
```

Limitation: actions the engine never picks naturally (e.g. rare enrage
poses) never enter the cache, so they can't be pinned. Eager resolution
(call vt[8] directly to populate the cache) is deferred because vt[8] is
a 95-insn compound function with VFPU writes and a small RNG path — calling
it speculatively would perturb engine state. See "What full RE costs" below.

## What full ptr-resolver RE costs

If a future mod needs eager resolution (force an action the engine has
never picked) the work is:

1. Disasm vt[8] @ `0x08865254` — full ~95 insns. ~1-2h.
2. Trace data flow against the live `entity+0x1AC` structure (decoded as
   `(u32 weight, u32 value)` pairs + `0xFFFFFFFF` sentinels in §32h, but
   the value→ptr mapping needs more work). ~1-2h.
3. Identify the input→output computation: is it `table_base + offset` or
   weighted-random across a bucket or nested `lookup(input).sub_table[idx]`?
   ~1-2h.
4. Replicate as pure C `mhfu_action_ptr_for_eager(entity, input)` that
   mirrors the lookup without VFPU / RNG. ~1h.
5. Validate against `tools/out/tigrex_inputs/observed.txt` (54 known
   pairs). ~30min.

Total: ~4-8h focused work. Open questions that may grow scope:
* Does vt[8] depend on entity state beyond `+0x1AC` (heading, stress,
  player distance)? If yes, the resolver needs the same state as input.
* Does the RNG path make different calls return different ptrs for the
  same input? Then "the resolved ptr" isn't a function — would need to
  expose the bucket of candidates.

For now: passive cache is good enough for the vast majority of mods.

---

# Section 32j — Big-monster slot loop body + body-part theory + freeze-gate (2026-05-31)

Building on the action_decided framework from §32d-32i. While building the
`tigrex_spin` mod (replace Giadrome quest's monster with Tigrex, lock
into TIGREX_ANGRY_SPIN after first natural pick, release on HP drop) we
disasm'd the per-tick slot loop body and identified the engine-side
state cells that gate AI tick advancement.

## Slot loop body — EBOOT `z_un_08865648` (0x088656A0..0x08865730)

Per-tick driver. Body:

```mips
0x088656A0  lhu  $v0, 0x1a2($s2)   ; slot count = entity+0x1A2 (u16)
0x088656A4  beqz $v0, end
0x088656A8  move $s1, $zero         ; s1 = slot = 0
0x088656AC  move $s0, $s2           ; s0 = entity (input cursor base)

LOOP @ 0x088656B0:
0x088656B0  sll  $v0, $s1, 6        ; v0 = slot * 0x40
0x088656B4  addu $v0, $s2, $v0
0x088656B8  lb   $v0, 0xbe($v0)     ; SKIP FLAG @ entity + slot*0x40 + 0xBE
0x088656BC  bnel $v0, $zero, tail   ; if non-zero, slot is busy → skip

0x088656C4  addu $v0, $s2, $s1
0x088656C8  lbu  $v0, 0x1a8($v0)    ; ENABLE BYTE @ entity + slot + 0x1A8
0x088656CC  beqz $v0, tail          ; if zero, slot disabled → skip

0x088656D4  lw   $t9, ($s2)         ; vtable
0x088656D8  lhu  $a1, 0x1b8($s0)    ; INPUT @ entity + slot*2 + 0x1B8 (u16)
0x088656DC  lw   $t9, 0x20($t9)     ; vt[8]
0x088656E0  jalr $t9                 ; CALL 1 — gate
0x088656E4  move $a0, $s2            ;   delay
0x088656E8  beqz $v0, tail          ; gate returned 0 → skip slot

0x088656F0..0x088656FC               ; CALL 2 — same args, descriptor result
0x08865700  move $a0, $s2

0x08865704  lw   $a0, 0x190($s2)    ; entity+0x190 = anim struct ptr
0x08865708  move $a1, $v0            ; vt[8] return = action ptr
0x0886570C  move $a2, $s1            ; slot index
0x08865710  jal  0x885f9a0          ; install_action(anim_struct, ptr, slot, $a3=1)
0x08865714  addiu $a3, $zero, 1

tail @ 0x08865718:
0x08865718  lhu  $v0, 0x1a2($s2)
0x0886571C  addiu $s1, $s1, 1        ; slot++
0x08865720  sltu $v0, $s1, $v0
0x08865724  bnez $v0, LOOP
0x08865728  addiu $s0, $s0, 2        ; input cursor += 2

end @ 0x0886572C:
0x0886572C  jal  0x88637c4          ; scripting slot processor
0x08865730  addiu $a0, $s2, 0x80
```

**Per-slot inputs**:
* `entity + slot*0x40 + 0xBE` u8 — skip flag (busy)
* `entity + slot + 0x1A8` u8 — enable byte
* `entity + 0x1B8 + slot*2` u16 — input cursor for vt[8]

Big-mon overlay path `0x09AC52DC` has the same structural pattern; the
slot count is loaded from `entity+0x640` instead of `+0x1A2`. Both
patched live via Section 26 quiet-gate; overlay body
(0x09AC52E4..0x09AC54C0) is JIT-overwritten and can't be disasm'd from
RAM, but symmetry to the EBOOT path is high.

**vt[8] is called TWICE per slot per tick**. Both calls use identical
input. CALL 1 is a probability gate (return 0 → engine `beqz` skips slot).
CALL 2 returns the actual install ptr. Implications: action_decided
post-hook (the framework event) fires twice per slot per tick.

## vt[8] internals — `z_un_08865254` decoded (95 insns)

Pure deterministic probability picker. No RNG read in the function.
Input `a1` decomposed:

| Slice           | Use                                |
|-----------------|------------------------------------|
| `a1 / 1000`     | category index                      |
| `(a1 % 1000) / 100` | table row index ×8 stride       |
| `a1 % 100`      | probability threshold               |

Algorithm:
1. `a2 = entity+0x1AC` (action table ptr). If null → return 1000 (default).
2. Compute `idx = (a1 % 1000) / 100 * 8`.
3. Load `v0 = action_table[idx]`. If `(a1 % 100) < v0` → return `action_table[idx + 4]` (success ptr).
4. Else fallback: `v1 = action_table[443 + (a1 % 100) * 4]`. If `v1 != -1` → return `action_table + v1`. Else 0.

Pure function of `(entity+0x1AC, a1)`. Same input → same output. → The
framework's passive cache (snoop on action_decided) is safe.

## Slot = body part (verified)

Three slots per big monster; each slot drives one body segment. A
coordinated full-body action requires all 3 slots playing the action's
per-part descriptor concurrently. SPIN's natural per-slot inputs (live
captured on Tigrex):

| Slot | Input  | vt[8] returns (Tigrex em13 model)         | Likely role     |
|------|--------|-------------------------------------------|-----------------|
| 0    | 0x0413 | 0x0946ECFC                                | Head / front    |
| 1    | 0x04DB | 0x094E8360                                | Body / mid      |
| 2    | 0x05A3 | 0x0950DBC8                                | Tail / spinner  |

Stride 200 between slot inputs is consistent across actions (general
pattern observed in polling — e.g. another action's triple was
`(1017, 1217, 1417)`). The 200 stride is the engine's per-slot offset
into the action table.

Forcing slot-2-only SPIN: tail spins indefinitely while head + body
do unrelated picks → visual chaos. Forcing the full triple under a
naive override defeated the per-slot natural gate (vt[8] CALL 1 → 0
when slot idle) → install thrash → AI tick rate dropped to 0.7% normal.
The working pattern: cache all 3 slots' natural SPIN ptrs from the
arming-frame action_decided event (engine already iterates 0→1→2),
then return cached ptrs only when `engine_value != 0` (preserves gate).

## Per-entity state cells — freeze gate

Engine accumulates "exhausted" bits during forced repeat-fire. After
3-4 SPINs the AI tick halts entirely (per-entity frame counter
`entity+0x092` stops advancing). The cells the engine zeroes / sets
during exhaustion:

| Offset       | Type | Role                                             |
|--------------|------|--------------------------------------------------|
| `+0x1A8..0x1AA` | u8×3 | per-slot enable bytes — engine zeroes after exhaustion |
| `+0x1B8/A/C` | u16×3 | per-slot input cursors — engine zeroes after exhaustion |
| `+0x29C`     | u32  | stress-response output mode (0 during SPIN)       |
| `+0x32C`     | u16  | AI param scratch (0 during SPIN)                  |
| `+0x414`     | u32  | bitfield — climbs `0xa → 0x12c` over 3 SPINs (red herring; not the freeze cause) |
| `+0x4B8`     | u32  | ⭐ **THE freeze gate**. Baseline 0. Engine OR's bits `0x100 \| 0x10000` during forced spins. Halts AI tick when set. |
| `+0x54E`     | u16  | natural play decrementer (1225 → 0); red herring |
| `+0x550`     | u16  | stamina-like counter (9000 baseline); secondary gate, decays ~10 per 333ms |
| `+0x648/4C`  | f32  | paired (450 / 1000) — likely rage cur/max         |

The `tigrex_spin` v0.12 mod refreshes `+0x4B8 = 0` and re-primes input
cursors + enable bytes + `+0x550 = 9000` on every action_decided fire.
Once `+0x4B8` is held at 0 the engine never enters exhausted state →
SPIN repeats indefinitely until HP drops (release on first HP delta).

## Slot container layout (`entity + 0x80 + slot*0x40`, stride 0x40)

All 3 slot containers have identical shape; vary only in `+0x38` (the
installed anim ptr written by `z_un_0885F9A0`):

| Container offset | Type | Notes                                         |
|------------------|------|-----------------------------------------------|
| `+0x10`          | f32  | per-slot timer (observed 56.0 → 332.0 evolve) |
| `+0x14`          | f32  | per-slot scalar (1.25f default)               |
| `+0x24`          | f32  | per-slot scalar (0.25f default)               |
| `+0x38`          | u32  | installed action ptr — the slot's anim         |
| `+0x3C`          | u32  | flag word (=0x1 after install)                 |
| `+0x3F`          | u8   | running flag (0 = idle, ≠0 = playing)          |

`mhfu_action_ptr_for(monster_type, vt8_input)` from §32i resolves to
exactly the `+0x38` value the applier writes.

## Methodology — reusable "find the engine's freeze gate" recipe

Each step has a one-shot script in this session's `/tmp/`. Repeat for
any entity-level wedge:

1. **Capture baseline** (`/tmp/baseline.py`) — full 31KB entity dump
   (stride 0x7A00 for Tigrex; 0x4A60 for Popo; 0x4530 for Anteka) to
   `/tmp/<entity>_baseline.bin` BEFORE the wedge condition.
2. **Wide static scan** (`/tmp/wide_scan.py`) — sweep the baseline for
   paired f32 cur/max signatures, u16 counters, isolated bit flags
   across the full stride. Surfaces stamina/HP/timer candidates.
3. **Live diff poll** (`/tmp/live_diff.py`) — 3 Hz x 6 min logging every
   cell change vs baseline. Lets the engine wedge naturally while
   recording. Post-analyze for monotonic ratchets.
4. **Unfreeze probe** (`/tmp/unfreeze.py`) — for each top suspect, one
   shot write the baseline value; watch `entity+0x092` for advance
   within ~1.5s. The cell that unfreezes is the gate.
5. **Bisect** (`/tmp/find_gate.py`) — when single-cell probe is null but
   bulk reset of all diff cells unfreezes, binary-split the diff set.
   Found `+0x4B8` in 5 iterations from 52 candidates.

The bisect step is the key new tool. It scales to any single-cell-gate
mystery without requiring upstream RE.

---

# Section 32k — The coherent action-force seam (executor 0x09AC5228), shipped & verified (2026-06-03)

The prior force surfaces (`action_input` = vt[8] input mutate; `action_decided`
= vt[8] return swap) both fire **per slot** and only reliably see **one slot**
for true-big-mon (tigrex), so forcing through them desyncs the body parts →
crash (§32j). This section pins the single upstream decision point and ships a
crash-free force hook around it.

## The executor `0x09AC5228` (overlay big-mon)

RE'd by mem-write-BP on `entity+0x324` (the live per-slot action triple) →
writer PC `0x09AC5458` (`sh $s2,0x324($s0)`), then clean `memory.disasm` of the
enclosing function (raw RAM reads return JIT `0x68XX` markers — use the
debugger's disassembler). Tools: `tools/re_base_decision_writers.py`,
`tools/re_disasm_slot_loop.py`, `tools/re_selector_640_writer.py`,
`tools/re_executor_a1_livetest.py`; dumps in `tools/out/tigrex_base_writers/`.

Function = `f(a0=entity, a1=action_id, a2, a3)`:

```
0x09AC5228  addiu sp,sp,-0x40 ; sw ra,0x2C(sp) ...     ; prologue
0x09AC5258  sw a1, 0x3C(sp)                            ; save the action id
            ; (if entity+0x638 bit0: remap idx via descr[a1].byte3,
            ;  sw 0x09BD38F0 -> entity+0x640  = the type-0x3A table)
0x09AC52D8  sll fp, <row idx>, 3                        ; fp = action_row * 8
0x09AC52DC  loop: a2 = fp + [entity+0x640]              ; -> descriptor row
            X = descr[0]                                ; action-id byte
            s2 = (X + 0x3E8 + slot*0xC8) & 0xFFFF       ; per-slot vt8 input
            sh s2, 0x324(s0)                            ; -> +0x324/6/8 (s0+=2)
            t9 = entity->vtable[0x20]; v0 = t9(entity,s2) ; vt[8] resolve
            if v0: z_un_08863e70(...)                   ; applier
            ; loop s3++, s1+=0xC8, s0+=2, s6+=0x40 while s3 < [entity+0x1A2]
```

**`a1` IS the action id.** The three per-slot cells `+0x324/6/8` are *derived*
from one byte as `X + 0x3E8 + slot*0xC8` — coherent by construction, which is
exactly why poking one slot desyncs. Override `a1` at entry and the engine fans
*your* action to every body slot through its own vt[8] + applier.

Relation to the documented inputs: `a1 = vt8_input(slot2) − 0x578`
(= `vt8_input(slot0) − 0x3E8`). Tigrex: ANGRY_SPIN `0x05A3→a1=0x2B`,
IDLE_STAND `0x0598→a1=0x20`, IDLE_WALK `0x057B→a1=0x03`, ANGRY_CHARGE
`0x0589→a1=0x11`.

The "brain" is a state machine in the `0x09D2xxxx` overlay keyed on
`entity+0x1D5`; per state it calls `jal 0x09AC5228` with a hardcoded/computed
`a1` (e.g. caller `0x09D26558`: `li a1,0x33; jal 0x09AC5228`). Live-verified:
`a0=Tigrex`, `a1=0x33` ⇒ slot0 input `0x041B` = `0x33+0x3E8`.

## The hook — `mhfu_on_bigmonster_action`

`include/mhfu/ai.h` + `src/core/ai.cpp`; Lua `mhfu.on_bigmonster_action(fn)`
where `fn(ctx{entity,type,action_id}) -> action_id`. Entry-detour on
`0x09AC5228`: cave wrapper saves `a0/a2/a3/ra`, calls
`mhfu_bigmonster_action_dispatch_c(entity,a1)`, puts the return in `$a1`,
replays the displaced prologue (`addiu sp,-0x40` / `sw ra,0x2C(sp)`), resumes
`0x09AC5230`. Filtered by `mhfu_entity_is_big_monster` (the executor is
generic — also fires for other entities). Lua path marshals to the exec thread
(game-thread Lua-corruption fix).

## Three things that make it actually work on PPSSPP

1. **Re-patch survives section roams.** Overlay code is JIT-fragile. The
   overlay-loaded helper (`mhfu_ai_overlay_loaded_helper`, postfix on loader
   `jal 0x0884E9F8`) no longer one-shots: whenever the probe at `0x09AC5228`
   reads the **original** prologue `0x27BDFFC0` (fresh/re-memcpy'd overlay,
   JIT-cold) it re-arms + re-applies the patch. Plus a `map_section_entered`
   re-patch fallback. `install_overlay_action_hook` reuses the cave wrapper
   (no leak) and only patches when bytes are original (never over a marker —
   Section 25 trap).
2. **`read_u32(0x09AC5228) == 0x68XX` is NOT a dead hook.** PPSSPP writes its
   JIT EMUHACK marker back into code RAM after translating **our** `J`;
   execution still routes through the wrapper. (Earlier "hook silent after
   roam" was actually the freeze gate, not a dead patch.)
3. **Freeze gate `entity+0x4B8` (§32j).** Forced repeat-fire makes the engine
   OR in `0x10000 | 0x100` (observed `frz=0x00010100`) → AI tick halts →
   executor stops being called → monster frozen (aggro/eye/stamina persist).
   Clear `+0x4B8` each fire in the cb, **and** from the worker `mhfu_tick`
   (the halted AI tick can't run the per-fire clear, so the worker is the
   live unfreeze net).

## Verified live (2026-06-03, HITL, Giadrome-REPLACE quest, cold boot)

A **visible** engaged Tigrex locked into **continuous ANGRY_SPIN**: no crash,
aggro intact (yellow-eye + run-away stamina drain persist = engine
side-systems untouched), moves + spins sustained. Reference mod:
`mods/lua_host/scripts/tigrex_spin.lua`.

## Descriptor table = the move "database" (no names)

`entity+0x640` → base ptr (`0x09D5A580` = `em75.ovl`/`file_06108 + 0x40440`;
remap `0x09BD38F0` when `entity+0x638` bit 0 set). 8-byte rows indexed by `a1`
(`base + a1*8`).

**LIVE-VERIFIED layout (2026-06-28, full table dumped off the running Brute):**
the Tigrex table is **123 rows** (a1 `0x00..0x7A`; at a1=123 the `b0` self-index
ramp restarts → the next block). Each row:

| byte | meaning |
|------|---------|
| b0 | `= a1` (self-index ramp, verified 0x00..0x7A) |
| b1 | category: `0xFF` normal (121 rows) / `0x01` (only a1 24,25) |
| b2–b5 | `0x00` |
| b6,b7 | two small even params (0,2,4,6,8,10,16) — purpose TBD (not the clip) |

**The clip slot is NOT stored in the row — it is DERIVED from `a1`:** the applied
per-slot input is `a1 + 0x3E8` (live `entity+0x324` = 1005 ⇒ a1 5), resolved by
vt[8] `0x08865254` → clip ptr in the `entity+0x1AC` table. So **to reach a clip you
just force its `a1`** (no row authoring); the row only needs to *exist* with a valid
category.

**Consequence for porting:** the table has **123 action rows ≥ the ~100 clip slots**
in a big-mon anim pack, so on the Tigrex host **every clip is reachable by forcing
some `a1` 0..122 — no descriptor injection is needed to make a ported moveset
accessible to Lua AI scripting.** Descriptor injection only matters for (a) a host
with fewer rows than your clips, or (b) custom **hitboxes** (NOT in this row — separate
data, see hitzone RE). **Binary only** — no human-readable move strings; the
`<SPECIES>_ACTION_*` names are hand-authored by watching forced actions.

Tooling: `anim table <slot>` dumps this table live; `anim sweep <slot> [lo hi]`
force-cycles `a1` via `on_bigmonster_action` and logs the resulting clip ptr to build
the `a1 → clip` map (`src/mhfu_bot/cli/commands/moveset.py`).

## Auto-paint map cheat (locate a roaming monster)

EU CWCheat "Auto-Paint Bosses" `_L 0x008B3A6A 0x000000FF` → real `0x090B3A6A`,
u8 `0xFF`, re-written ≥2 Hz (`mhfu.write_u8(0x090B3A6A,0xFF)` in `mhfu_tick`).
Marker shows the logical position even when the monster is render-unbound
(`entity+0x008==0`).

---

# AI override-event API detail (migrated from CLAUDE.md, 2026-06-05)

**AI override-event API (Section 32d–32i, 2026-05-30; action-force seam
2026-06-03, all verified live).** The framework exposes priority-chain
sync-override events on the generic monster-AI engine — `mhfu/ai.h`.

* **★ Action force (COHERENT OVERRIDE — use this to force big-mon actions)**
  — `mhfu_on_bigmonster_action(cb, priority)` (Lua `mhfu.on_bigmonster_action`).
  Entry-detour on the big-mon action **EXECUTOR** `0x09AC5228` =
  `f(entity, a1 = action_id, …)`. `cb(ctx{entity,type,action_id}, a1) -> a1`.
  The executor fans the returned id to **all 3 body-part slots itself**
  (per-slot input = `a1 + 0x3E8 + slot*0xC8`) through the engine's own vt[8]
  resolver + applier → **coherent, no desync, no crash**, and aggro/music/
  stamina side-systems keep running. `a1 = vt8_input(slot2) - 0x578`
  (= slot0_input − 0x3E8); e.g. Tigrex ANGRY_SPIN `a1=0x2B`, IDLE_STAND
  `a1=0x20`. **VERIFIED 2026-06-03: visible Tigrex locked into continuous
  ANGRY_SPIN, no crash, no freeze.** Three gotchas, all handled by the
  reference mod `mods/lua_host/scripts/tigrex_spin.lua`:
  1. **Re-patch survives roams.** It's overlay code (JIT-fragile). Installed
     from the overlay-loaded helper, which now RE-FIRES (one-shot dropped) →
     re-applies the patch in the JIT-cold window after a section roam
     re-memcpy's the overlay. Plus a `map_section_entered` re-patch fallback.
  2. **`read_u32(0x09AC5228) == 0x68XX` marker ≠ dead hook** — it's PPSSPP's
     JIT translation of OUR `J`; execution still routes through the wrapper.
  3. **Freeze gate `entity+0x4B8`** — forced repeat-fire makes the engine OR
     in `0x10000|0x100` and halt the AI tick (frozen, aggro persists). Zero
     `+0x4B8` each fire AND from a worker-thread net (the halted AI tick
     can't run the per-fire clear, so the worker revives it live).
* **Picker / per-slot resolver (OBSERVE; SUPERSEDED for forcing)** —
  `mhfu_on_bigmonster_action_decided(cb, priority)`: vt[8] swap on shared
  `0x08865254`, JIT-immune. `cb(ctx, engine_value) -> new_value`. For
  small-mon-shape (POPO/ANTEKA/GIADROME) it returns a flat u16 action id and
  still works to force; for **true-big-mon (TIGREX) it fires PER-SLOT** and
  forcing through it desyncs the body → **crash** — use `on_bigmonster_action`
  instead. `on_bigmonster_action_input` (vt[8] input mutate, same caveat:
  only one slot calls back → desync) is likewise superseded for forcing.
  Keep these for telemetry / small-mon.
* **Per-frame tick (OBSERVE)** — `mhfu_on_bigmonster_ai_step(cb, priority)`:
  entry detour on `z_un_08865648`. NOT for action override (engine already
  uses vt[8] inside this function); for telemetry, distance checks, etc.
  Quiet-gated at TITLE/MENU (Section 26 JIT bypass). Verified live: word
  @0x08865648 = `J 0x09D87F50` (`0x0A761FD4`), word @0x0886564C = NOP.
* **Lifecycle** — `mhfu_on_bigmonster_spawn` / `_death` (observe, 5 Hz poll).
* **Dropped**: `anim_decided` — RE showed `z_un_0885f928` does a slot-index
  tree lookup returning the per-slot state-container, not a transform of
  vt[8]'s output (docs/AI_SCRIPTING_ENGINE.md §32g).

**Two AI classes** (`docs/AI_SCRIPTING_ENGINE.md` §32h):
| Class | Species | vt[8] returns | Override pattern |
|-------|---------|---------------|------------------|
| small-mon-shape | POPO, ANTEKA, **GIADROME** | u16 action ID (zero-ext u32) | return `<SPECIES>_ACTION_*` macro value |
| true-big-mon | TIGREX | POINTER into entity+0x1AC sub-table | return ptr from cache; see below |

Quest UI lumps tigrex+giadrome as "big monster" (counted as targets); the
internal AI tier differs. Tigrex needs richer per-action data (anim group,
hitbox geom, range floats), so engine uses descriptor pointers instead of
flat IDs. **The per-slot `vt[8] returns` distinction above matters for the
SUPERSEDED action_decided path; the recommended `on_bigmonster_action` seam
sidesteps it entirely — you return one flat `a1` action id for either class
and the executor handles the fan-out + (for tigrex) the ptr resolution.**

**Action descriptor table** (the engine's per-species move "database", RE'd
2026-06-03): base ptr at `entity+0x640` (`0x09D5A580`; type-0x3A remap table
`0x09BD38F0` swapped in when `entity+0x638` bit 0 set). 8-byte rows indexed by
`a1`: `+0` action-id byte, `+1` slot/body-part kind, `+3` remap index, `+4`
param (→`+0x32C`), `+6` param (→`+0x334` mode). **Binary only — NO
human-readable move names exist in the game.** The `<SPECIES>_ACTION_*` /
`TIGREX_VT8_INPUT_*` names in `ai_actions.h` + the `LABEL` table are
hand-authored by observing forced actions in-game. Dump the table + force-cycle
`a1` via `on_bigmonster_action` to enumerate + name a species' full moveset.

**Auto-paint map cheat (find a roaming monster):** EU CWCheat "Auto-Paint
Bosses" `_L 0x008B3A6A 0x000000FF` → real addr `0x090B3A6A`, write u8 `0xFF`
continuously (≥2 Hz). Applied live from Lua via `mhfu.write_u8(0x090B3A6A,
0xFF)` in `mhfu_tick`. Shows big monsters on the in-game map like a paintball.
Marker shows the LOGICAL position even when the monster is not being drawn
(e.g. a swap-spawned Tigrex culled by the `+0x29A` section-mismatch gate, or one
genuinely in another section).

**Species action enums** (`mhfu/ai_actions.h`, regenerated from tigrex_s6):
* `POPO_ACTION_0x????` — 13. `ANTEKA_ACTION_0x????` — 40.
* `GIADROME_ACTION_0x????` — 34. `TIGREX_VT8_INPUT_0x????` — 50 (+4 probe).
* Tigrex's `+0x0C` species data is **hitzone**, not an action list, so it
  gets `TIGREX_VT8_INPUT_*` (stable u16 inputs) instead of action IDs;
  the ptr the engine resolves to is per-run.
* `mhfu_action_is_valid(type, id)` runtime-walks the species table.

**Tigrex ptr-resolution helper** (Section 32i, passive cache):
```c
uint32_t mhfu_action_ptr_for(uint8_t monster_type, uint16_t vt8_input);
```
Framework snoops every `(input, ptr)` pair from `action_decided` callbacks
into a 96-entry-per-species cache (even with zero subscribers). Mods call
the lookup with a `TIGREX_VT8_INPUT_*` macro and get the engine's last
resolved ptr — or 0 if the engine hasn't picked that action yet this run
(cache warmup). Eager resolver (calling vt[8] directly) deferred because
vt[8] has VFPU + small-RNG side effects; passive cache covers ~99% of
practical mod work.

**Other SDK helpers**:
* `mhfu_monster_name(type)` (`mhfu/ids.h`) — `"POPO"` / `"ANTEKA"` /
  `"TIGREX"` / `"GIADROME"`, or `"0xNN"` fallback. For logging.
* `mhfu_action_cache_size(type)` + `mhfu_action_cache_entry(type, i, …)`
  for cache introspection.

Reference mod: `framework/prx/mods/experimental/ai_demo/mod.cpp`.

---

## §33 — Big-monster COMBAT-LATCH / notice→aggressive escalation (2026-07-01)

RE'd why a **species-swapped** big monster (Giadrome→Tigrex; native model via
`brute_tigrex.lua CAPTURE_NATIVE=true`) roar-loops and never fights, using the fork's
slot-7 **native** Tigrex (charges ~6 s after load) as the working reference. This is the
layer ABOVE the action executor — the high-level AI state machine that decides to ENTER
combat.

**Brain state machine (Tigrex, overlay `0x09D26xxx`):**
- **Outer AI state = `entity+0x299` (u8, 0..0x21).** The per-frame brain driver
  `0x09D33EA0` reads it, bounds-checks (`sltiu at,v1,0x22`), and `jr`s through the jump
  table **`0x09D626F0`** (`[base + state*4]`, `jr v1` @`0x09D33ECC`) to that state's
  handler. Native and swap were BOTH `+0x299=4` → same handler → the divergence is a data
  flag, not the dispatch.
- **Inner brain phase = `entity+0x1D5` (u8).** Each state-handler sub-switches on it.
- **State-4 handler `0x09D26648` = roar→charge:** phase 0 fires the **ROAR** through the
  action executor `0x09AC5228` with **a1=0x36**, sets `+0x523=1`; phase 1 waits for the
  roar anim (`+0x76A` tracker); phase 2 → **escalation gate** → `+0x1D5=3` (`sb`
  @`0x09D2675C`) + `jal 0x09D26158` (**combat-enter**: sets `+0x4B5=1`, dispatches
  `entity->vt[0x88]`) → the monster charges.

**THE ESCALATION GATE = `entity+0xBC` bit 0.** The phase-2 path (`0x09D26744`):
`lhu +0xBC; andi 1; bne !=0 → skip`; it writes `+0x1D5=3` only when **bit0 == 0**.
`+0xBC` is a **fast-toggling per-frame flag**. On the **native** it reaches 0 often at
brain 2 → escalates reliably. On the **swap** it is stuck at **1 in 44/45 brain-2
frames** → the escalation almost never fires → the monster loops roar↔notice forever
(`+0x1D5` oscillates 1↔2, never 3; `+0x4B5` never sets).

**Proven:** force-clearing the swap's `+0xBC &= ~1` at high rate DID fire combat-enter
(`+0x4B5` reached 1), but the engine **re-sets bit0 every frame**, so the clears race it →
on-screen the monster "wiggles, completes no animation." Force-poking confirms the gate
but is not a clean fix — there is a per-frame WRITER of `+0xBC` bit0 whose condition holds
on the swap but clears on the native (lead: **roar-completion**; the roar tracker `+0x76A`
differed native-vs-swap, and brain phase 1 waits on it → the swap's roar never "completes"
→ bit0 never clears).

**Retractions / corollaries:** the ROAR itself dispatches through the executor
`0x09AC5228` (a1=0x36), so a swap showing "0 executor calls" via the framework detour =
the **detour was dead on that instance** (JIT-wiped), NOT the executor being absent.
Target also differs (native→player `+0x2F4=0x090B3440`; swap→cat `0x090BDC40`) but forcing
target never helped → symptom, not cause.

**Open / next:** write-BP `+0xBC` on the swap to find the per-frame bit0 SETTER + its
condition (why perpetually "not ready" on the swap); compare to native. That points to
either a real fix (satisfy roar-completion / the missing wiring) or confirms it's
spawn-time combat wiring → the native add-target/relocate spawn path.

**Key cells:** gate `+0xBC` bit0, brain `+0x1D5`, outer state `+0x299`, combat-enter flag
`+0x4B5`, roar tracker `+0x76A`. **Code:** dispatcher `0x09D33EA0` (tbl `0x09D626F0`),
handler `0x09D26648`, escalate write `0x09D2675C`, combat-enter `0x09D26158`,
roar-via-executor `0x09AC5228 a1=0x36`.

**Tooling note:** the fork's debugger **`savestate.load` command did NOT reload** here
(no-op). Reliable reload = relaunch `--state slot7` (`lifecycle.launch_ppsspp(state=)`),
which loads the slot DIRECTLY (fast, no full boot). Native slot 7 = `ULES01213_1.01_6.ppst`
(area 109); charges ~6 s after in-area then dies to an Anteka → basecamp (reload to see it
again). Full narrative → memory `swap-bigmon-combat-latch-gate` (sessions 3/3b/3c).

### §33b — UPDATE (2026-07-01, session 4): `+0xBC` is a SYMPTOM; combat-enter IS reached but resolves to RE-ALERT

The write-BP on `+0xBC` **supersedes the "gate = `+0xBC` bit0" framing above.** bit0 is
honest animation state, and the escalation is NOT actually stuck — it fires and lands in
the wrong place.

- **`+0xBC` bit0 = "root-bone clip still playing this frame."** Per-frame SETTER =
  **`0x088639B0`** in the EBOOT anim/clip interpolator (`0x08863xxx`), NOT the overlay
  brain: `sh v0,0x3C(s1)` with `s1 = ent+0x80` (root-bone clip state; `+0x3C(s1) =
  ent+0xBC`). It **unconditionally ORs bit0** whenever the clip has not passed its end —
  gate `0x088638E0 c.le (phase+speed), clip_end`; when past end it takes the **clear path**
  (`0x08863A40`/`0x08863A68`: `andi 0xFFFE; sh`). Clip-state fields at `ent+0x80`: `+0x10`
  phase, `+0x14` speed, `+0x18` loop-restart, `+0x1C` end, `+0x3C`(=`+0xBC`) flags (bit0
  playing / bit1 loop / bit2 set on the roar). So bit0 clears cleanly at every clip's end;
  the swap just cycles roar(end=292)→short→short→roar, and bit0 only clears during the
  short clips while `+0x1D5`==1, never mid-roar while `+0x1D5`==2 → the `+0x1D5==2 &&
  bit0==0` coincidence the gate needs never happens. (A cat hit flinch-interrupts the roar
  → bit0 clears at a catchable moment → brief engage — HITL-confirmed.)
- **combat-enter `0x09D26158` IS reached** (stack-walk at a `+0x299` write-BP, live nested
  chain): `driver 0x09D33F1C → state-4 handler (ret 0x09D2676C) → combat-enter (ret
  0x09D261A4; jalr vt[0x88] @0x09D2619C) → combat_step vt[0x88] 0x09D3D608 → overlay
  dispatch 0x09D3D79C (idx = s2&0xFFFF into table 0x09D62D68; idx 0 → 0x09D3D7C8) →
  0x09D3C8D8 (action-dispatch on a1) → 0x09AC8690 → enter_state2 0x09AC87E8 → set_ai_state
  0x09AC8818(state=2)`. So the swap's virtual "combat step" **resolves to
  `enter_state2(state=2)` = RE-ALERT** (resets `+0x1D5=0`, sets `+0x4B7=1`) → the roar→charge
  phase machine restarts → the loop. combat-enter passes **mode a2=2** (its `v0==2` branch
  `0x09D26184`); mode 2 → dispatch idx 0 → action-id switch default → the state-2 path.
- **`set_ai_state 0x09AC8818(a0, a1→+0x298, a2→+0x299)`** is the generic AI-state setter:
  writes `+0x298`/`+0x299`, zeroes `+0x1D5/6/7`, calls `z_un_08865cb8`. Wrapper
  `enter_state2 0x09AC87E8` clears `+0x769`, sets `+0x4B7=1`. (The §33 "`+0x4B5=1` =
  combat-enter" is imprecise — the reached path sets `+0x4B7`; `+0x4B5` stayed 0.)
- **Swap targets the CAT** (`+0x2F4=0x090BDC40`, `+0x542=1`), not the player
  (`0x090B3440`). vtable = `0x089BB69C`, `vt[0x88]=0x09D3D608`.

**Refined root:** the escalation FIRES but its `vt[0x88]` combat-step resolves to re-alert
because the mode/action combat-enter passes (=2) — and/or the target being the cat — selects
the "return to alert" dispatch branch. `+0xBC` is downstream coupling, not the lever (poking
it races the clip writer → "wiggle"). **Next (HITL + fork):** capture the same chain on the
native slot-7 Tigrex and diff (a) combat-enter's mode arg (`0x09D2617x`), (b) dispatch idx
`s2&0xFFFF` (table `0x09D62D68`), (c) `+0x2F4` target — the differing one is why native
charges. Still points to **Path 2 (native add-target/relocate spawn wires real combat)** as
the fix. **New code addrs:** clip-flag setter `0x088639B0` (+0xBC), clip state `ent+0x80`,
combat_step `0x09D3D608`, dispatcher `0x09D3D79C` + table `0x09D62D68`, action-dispatch
`0x09D3C8D8`, enter_state2 `0x09AC87E8`, set_ai_state `0x09AC8818`. Full narrative → memory
`swap-bigmon-combat-latch-gate` (session 4).

### §33c — ✅ SOLVED (2026-07-01, sessions 4d–4f, HITL): use the ADD path, not a bare SWAP

The combat-latch problem is not a gate to poke — it's that a **bare emId SWAP reuses the
quest's single, incompletely-provisioned target group**, so the monster can posture (roar/
notice) but never commits. A natively-**ADDED** monster gets a **fresh target GROUP + its own
manager + full provisioning** (`buildTargets → mgr_ctor → provision_driver → resource_reg`),
and **that fights + deals damage.**

- **API:** `mhfu_quest_add_monster(quest, id[, x, z])` (Lua `mhfu.quest_add_monster`) in a
  `QUEST_TARGETS_BUILDING` subscriber — appends a list-A node + splits the new monster into
  `target[1]` (group 1) + bumps `Quest+0x67C=2`. Same-family = resident overlay, no relocation.
- **Proven (native Tigrex quest):** add a 2nd Tigrex → BOTH the native and the added one deal
  real damage to the player (killed twice; combat states 8–22 targeting the player). The bare
  swap deals 0.
- **General path for a different-family quest (Giadrome→Brute), shipped in
  `mods/lua_host/scripts/dup_test.lua`:** the one AI-overlay slot follows the emId + the
  add-forge is disarmed, so a bare different-family add has no overlay → **REPLACE**
  Giadrome→Tigrex first (overlay+node0 = Tigrex), **ADD** a 2nd Tigrex (same family now → the
  fighter), **suppress the primary** (teleport off-map + clear the visible bit). Ride the
  **Brute v63 model inject** (`inject_relocate 6185`) on top → the ported Brute renders,
  animates with his own moveset, engages, and kills. No crash from inject+replace+add.
- **Visibility fix:** the added fighter spawns in the intro section, not the player's; forcing
  `+0x29A` at the 2 Hz `mhfu_tick` can't hold (the engine re-derives the section tracker from
  POSITION each frame). A **one-time physical relocate** into the player's section STICKS (the
  engine then maintains it) AND kicks it from idle into combat.
- **Gotchas:** (1) forcing a specific action via `on_bigmonster_action` SETS the freeze gate
  `+0x4B8` (0x100|0x10000) faster than a 2 Hz clear → the monster freezes ("stuck standing") —
  let natural combat run instead. (2) `cli_bridge.lua` hooks `on_bigmonster_action` at priority
  90; a lower-priority force loses the chain. (3) **hitbox↔anim DESYNC:** v63 ships the Brute's
  OWN clips but the Tigrex overlay drives the actions/hitbox timing → an attack hitbox can be
  active during a non-attack pose ("killed by touching him"); ship **v58 (Tigrex-rig retarget)**
  for synced hitboxes, **v63** for authentic moves. **Timer freeze:** hold `0x09A05DD0` +
  `0x09A050F8` (~2 Hz). Full detail → memory `swap-bigmon-combat-latch-gate` (§4d–4f).

