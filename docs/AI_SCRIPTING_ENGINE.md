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

## Open follow-ups (next session)

1. **Pin the slot installer** — mem-BP `entity+0x122` AND `entity+0x110`
   during cutscene START (not mid-cutscene). Capture writer PC. That
   PC + its args = the API for activating scripted slots.
2. **Slot data format** — re-RE `z_un_088637C4`'s sub-callees
   (`z_un_0885fb54`, `z_un_0885ff5c`, `z_un_08862fe0`) to fully
   decode each slot field.
3. **Pin EU Evdemo singleton** — static scan for a 0xA0-byte
   structure pattern in `0x09A40000..0x09A60000`. Verify by trying
   `func_eboot_088D0824(ev, 0, 0)` for benign demo_id.
4. **Verify normal AI usage** — sample `entity+0x122` for popo while
   in normal aggression. If non-zero at any moment, the slot system
   IS used for combat AI, not just cutscenes.
5. **Map `state byte = 3`** at `entity+0x334` — observe when else it
   appears; correlate with anim ID.
