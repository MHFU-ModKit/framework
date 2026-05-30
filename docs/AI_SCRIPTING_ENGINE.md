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

The two hooks are EBOOT-resident. Section 26 JIT bypass (install at
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
