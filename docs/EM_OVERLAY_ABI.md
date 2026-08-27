# The engine ↔ big-monster-overlay interface (MHFU EU, ULES01213)

**Traced 2026-08-27, statically — no cold boot, no debugger.** Reproduce any table here with
`tools/em_abi.py`.

A big monster's AI is a per-species MWo3 overlay (`em*.ovl`). This document answers *how the
engine calls into one*, which is the piece that was never mapped: we knew a great deal about
em75's **insides** (`tools/em_moveset.py` constant-propagates through its handlers) and nothing
about its **entry surface**.

The answer is short: **the entry surface is a C++ vtable in the EBOOT, and every call the engine
makes into a species is a virtual dispatch through it.** Nine slots are overridden by all 17
species; those nine are the interface.

> Companion: `BIG_MONSTER_OVERLAY_RELOCATION.md` (the overlay slot + relocation),
> `AI_SCRIPTING_ENGINE.md` §33–34 (the two channels), `agent_memory_map.md` (addresses).

---

## 1. Why it *has* to be a vtable

`tools/em_abi.py inventory --em` lists all 17 species overlays:

```
distinct load addresses: ['0x9d1a180']
distinct ctor counts:    [0]
```

Every one of them loads at **the same VA**, `0x09D1A180`, and **not one has a static
constructor** (the MWo3 header's `static_init_start == static_init_end`). So the overlay never
runs any code to register itself — the engine cannot be *learning* the entry points at load time.
It must already know them.

It does: the EBOOT was linked against all 17 overlay images, and holds a **vtable per species**
whose slots point directly into overlay text. Loading a species overlay is nothing more than
dropping its code at the address the EBOOT's pointers already name.

That is also why a foreign overlay cannot simply be dropped in — see §7.

## 2. Vtable convention — ⚠️ two slot numberings exist in this repo

The vptr stored at `object+0` points at a **(offset-to-top, typeinfo) pair**, both zero in this
binary. Virtual function *k* therefore lives at `vptr + 8 + 4k`.

`em_abi.py` numbers **virtual functions**, so slot *k* is at `vptr + 8 + 4k`.
Older notes (`agent_memory_map.md`, `POPO_AI_STRUCTURE.md`) number **words from the vptr**, so
their slot *k* is at `vptr + 4k` and counts the zero pair as slots 0–1.

```
doc_slot = abi_slot + 2
```

Worked example — the memory map records *"tigrex monster vtable `0x089BB69C`, slot 8 (+0x20) =
`0x08865254`, the action picker"*. `0x089BB69C + 0x20 = 0x089BB6BC`, which holds `0x08865254`. In
`em_abi.py` terms that is **abi slot 6**. Both are right; only the convention differs.

## 3. The 17 species vtables

`tools/em_abi.py vtables`. Each vtable is found by scanning EBOOT `0x089B0000..0x089CB000` for a
zero pair followed by a run of code pointers; exactly **17** of them have 61 slots *and* reference
overlay code, which matches the 17 overlay species exactly.

Attribution is by disassembly: a vtable's overlay pointers are tested against every species image
for whether they land on a plausible function entry. Pointing at the wrong species lands
mid-instruction-stream, so the winner separates clearly (54–100% vs. a 5–21% runner-up), and the
mapping comes out **1:1 across all 17**.

| species | entity vtable | species | entity vtable |
|---|---|---|---|
| em01 | `0x089BBE7C` | em54 | `0x089BECA0` |
| em02 | `0x089BE6B0` | em55 | `0x089BEE08` |
| em07 | `0x089BE7F0` | em58 | `0x089BEF04` |
| em14 | `0x089BBD80` | em59 | `0x089BF0D8` |
| em15 | `0x089BBC84` | **em75 (Tigrex)** | **`0x089BB69C`** |
| em17 | `0x089BBB88` | em82 | `0x089BB5A0` |
| em20 | `0x089BBA8C` | em83 | `0x089BB4A4` |
| em21 | `0x089BB990` | | |
| em33 | `0x089BB894` | | |
| em40 | `0x089BB798` | | |

**Independent confirmation:** the static attribution lands em75 on `0x089BB69C`, which the project
had already recorded from *live reads* of the Tigrex entity's `+0x00`. Two unrelated methods, same
answer.

## 4. The interface — 9 mandatory slots

`tools/em_abi.py interface` diffs all 61 slots across all 17 species. A slot every species
overrides is one the base class cannot implement generically — i.e. part of the required
interface. A slot nobody overrides is inherited outright.

| abi slot | `vt+` | em75 impl | what it is |
|---|---|---|---|
| 4 | `0x018` | `0x09D3F338` | dispatched from a global singleton, 3 sites |
| 14 | `0x040` | `0x09D25F38` | 11 sites |
| 27 | `0x074` | `0x09D25FD0` | no `game_task` sites — called from EBOOT / `game_sub` |
| **29** | `0x07C` | `0x09D35328` | **the per-frame AI step** — 1463 insns, reaches the main-6 dispatcher `0x09D351E0`; returns a u8 the caller masks with `andi v0,0xFF` |
| 30 | `0x080` | `0x09D36C88` | called with two extra args (`a1=10, a2=0`), 9 sites |
| **32** | `0x088` | `0x09D3D608` | **enter-action** — dispatched with four args in the `act_set(entity, main, sub, mode)` shape |
| 55 | `0x0E4` | `0x09D3DBF8` | dispatched immediately after a write to `entity+0x638` (the visibility cell) |
| 57 | `0x0EC` | `0x09D3DCF0` | runs in the same per-frame function as slot 29, *before* it |
| 58 | `0x0F0` | `0x09D3DB68` | dispatched after `sh zero, 0x454(entity)` |

Near-universal but not mandatory — a base implementation exists and one or two species keep it:

| abi slot | `vt+` | n | base impl | not overridden by |
|---|---|---|---|---|
| 16 | `0x048` | 16/17 | `0x09AD0548` | em33 |
| 25 | `0x06C` | 16/17 | `0x09ACC340` | em33 |
| 31 | `0x084` | 14/17 | `0x09AC51F8` | em14, em17, em33 |
| 28 | `0x078` | 11/17 | `0x09AC75D0` | em02, em07, em33, em54, em55, em58 |

**14 of 61 slots are never overridden by any species** — pure base class.

The remaining optional slots are strongly clustered: em07/em15/em55/em58 all override the
`0x0AC..0x0C8` band (slots 41–48), which no other species touches. That band is an archetype's
extra interface, not a per-species quirk.

## 5. The per-frame call chain

The function at **`0x09AC5B2C..0x09AC68B0`** in `game_task.ovl` (865 insns) is the one that drives
a big monster's AI step. It makes exactly three virtual calls on the entity, in this order:

```
0x09AC5D04   vt+0x90  slot 34   (em75 ONLY — no other species overrides this)
0x09AC65A0   vt+0xEC  slot 57
0x09AC6638   vt+0x7C  slot 29   <- the AI step; `sh 2, 0x2BC(entity)` is written just before
```

Slot 29's return value is consumed as a byte at the other dispatch site (`0x09A6B0F8`), so it is a
status/continue flag rather than `void`.

⚠️ The *cadence* of this function (per frame? per fixed tick?) was not measured — the call order
is static fact, the timing is not.

## 6. `act_set` and slot 32 — how the semantics channel is entered

The four-argument dispatch of slot 32 sits in `0x09AC89E0`, a function whose single `jal` caller is
`0x09AC5A70` (itself reached indirectly, not by any `jal` in `game_task`). Argument shape at the
dispatch:

```
0x09AC8A3C  addu a1, s2, zero        ; main
0x09AC8A40  addu a2, s1, zero        ; sub
0x09AC8A44  lw   t9, 136(t9)         ; vt+0x88 = abi slot 32
0x09AC8A48  addu a3, s0, zero        ; mode
0x09AC8A4C  jalr ra, t9
0x09AC8A50  addu a0, s3, zero        ; entity
```

This ties the vtable ABI to the mechanism the project already drives from Lua: writing a
`(main, sub)` pair runs the species' **slot-32** implementation, which is the per-action code that
owns hitboxes and effects. It is the "behaviour channel" of `AI_SCRIPTING_ENGINE.md` §33, now
located precisely.

## 7. How a species is CHOSEN — the factory at `0x09AB15D8`

No EBOOT word anywhere holds a *pointer to* any of the 17 entity vtables, so there is no
species→vtable data table. The addresses only ever materialise as `lui`/`addiu` immediate pairs in
code — i.e. each species has its own constructor. All 17 of them turn out to live in **one
function**, `0x09AB15D8` in `game_task.ovl`, which is a plain jump-table dispatch:

```
0x09AB15E4  andi  v0, a1, 0x00FF        ; a1 = emId (u8)
0x09AB15EC  sltiu at, v0, 90            ; bounds check
0x09AB15F0  beq   at, zero, <default>
0x09AB15F8  lui   v1, 0x09C1
0x09AB1600  addiu v1, v1, -13064        ; -> 0x09C0CCF8, in game_task .data
0x09AB1604  addu  v0, v0, v1            ; + emId*4
0x09AB1608  lw    v0, 0(v0)
0x09AB160C  jr    v0                    ; -> the species case block
```

So **`0x09AB15D8(ctx, emId)` is the big-monster entity factory**, with a 90-entry jump table at
**`0x09C0CCF8`**. Its caller (`0x09AB2958`) loads the emId as `lbu 0(a1)` straight off a record.
Each case block allocates the entity (`0x08859B44`, **2048 bytes / 0x800**, align 16 — the same
0x800 span `tools/native_ai_probe.py` snapshots) and installs that species' vtable.

`tools/em_abi.py factory --check` walks each case block to its `sw <vtable>, 0(this)`:

**42 of 90 emIds route to one of the 17 overlays** — big-monster *variants* share their base
species' AI. Every species' primary emId equals its `em` number in decimal:

| overlay | emIds | overlay | emIds |
|---|---|---|---|
| em01 | 0x01, 0x0B, 0x25, 0x29, 0x2A, 0x31 | em33 | 0x21 |
| em02 | 0x02, 0x24, 0x47 | em40 | 0x28, 0x4E |
| em07 | 0x07, 0x32 | em54 | 0x36, 0x3C, 0x40, 0x41 |
| em14 | 0x0E, 0x1A, 0x2B, 0x2C | em55 | 0x37 |
| em15 | 0x0F, 0x2D | em58 | 0x3A |
| em17 | 0x11, 0x16, 0x2F | em59 | 0x3B |
| em20 | 0x06, 0x14, 0x26, 0x27 | **em75** | **0x4B (Tigrex)**, 0x4C, 0x51, 0x58 |
| em21 | 0x15, 0x2E | em82 | 0x52 |
| | | em83 | 0x53 |

Sanity checks pass: Tigrex `0x4B` → em75, and Giadrome `0x4D`, Popo `0x46`, Anteka `0x45` correctly
resolve to **no** big-monster class (their AI is in the EBOOT — consistent with §1 of
`BIG_MONSTER_OVERLAY_RELOCATION.md`).

> ⚠️ **Case blocks are only ~0x60–0xE0 apart.** An unbounded walk runs past the end of one block
> into the next and reports a *neighbouring* species. That is not hypothetical — the first pass
> here claimed Popo and Anteka were big monsters (em15 / em07) purely from overrun. The walk must
> stop at the next case start; `em_abi.py factory` does, and the four sanity checks above exist to
> catch a regression.

This is also a partial answer to the open question in `BIG_MONSTER_OVERLAY_RELOCATION.md` §4
("how a manager learns its species"): on the **entity** side, species comes from one byte fed to
one factory. Whether the *manager* task resolves species the same way is still untraced.

## 8. Classes an overlay defines itself

Beyond the entity vtable (which the *engine* installs), each overlay defines its own C++ classes
and installs their vtables from its own constructors — `sw <vtable>, 0(this)`.
`tools/em_abi.py classes em75`:

| overlay | classes it installs | EBOOT range |
|---|---|---|
| em75 | **17** | `0x089BF1D4..0x089BF4C4` |
| em54 | 6 | `0x089BEB90..0x089BED9C` |
| em58 | 4 | `0x089BEF04..0x089BF06C` |
| em17, em83 | 3 | |
| em02, em07, em33, em40, em55, em59, em82 | 2 | |
| em14, em15, em20, em21 | 1 | |

Each species' vtables occupy a **contiguous, disjoint slice** of EBOOT, ordered by species. em75 is
by far the most elaborate species: 12 eight-slot classes (`0x089BF1D4..0x089BF38C`) plus 5
fifteen-slot classes (`0x089BF3B4..0x089BF4C4`). Three slots of every eight-slot vtable point at
shared `game_sub` implementations (`0x09C56E68`, `0x09C56ED8`, `0x09C56F68`), so they all derive
from one `game_sub` base.

**Open:** what these 12+5 classes model. Their count and size make per-action or per-attack objects
a reasonable guess, but nothing here confirms it.

## 8. What this means for a hand-written overlay

The good news, in order:

1. **No init contract.** Zero constructors means nothing has to run at load.
2. **The interface is small and now enumerated** — 9 mandatory slots, 4 near-universal.
3. **The base class does most of the work.** 14 slots are never overridden and ~38 more are
   inherited by most species; a minimal species implements 9 functions and inherits ~52.
4. **We already know what the important two do.** Slot 29 is the AI step, slot 32 is enter-action.

The blockers, honestly:

- 🔴 **The vtable lives in the EBOOT, not in the overlay.** A hand-written overlay cannot bring its
  own — it must either reuse a species' existing vtable (and therefore be laid out so that its nine
  functions sit at exactly the VAs that vtable already names) or have the EBOOT vtable patched to
  point at ours. The second is a data write to EBOOT rodata, so it is *not* subject to the JIT
  code-patch trap (`ppsspp-debugging`), which makes it the tractable option — **untested**.
- 🔴 **The nine mandatory signatures are only partly known.** Slot 29 (`this`) → u8 and slot 32
  (`this, main, sub, mode`) are read off dispatch sites. The other seven have argument counts
  visible at their call sites but no verified semantics.
- ⚠️ **The engine↔overlay boundary is much wider than the entity vtable.** Calls go both ways, and
  the outbound side is the larger one: em75 `jal`s **212 distinct external addresses** — 108 in
  `game_task`, 52 in `game_sub`, 52 in the EBOOT. A custom overlay need not reimplement any of
  them, but it does have to call them with the right signatures, and only a handful of the 212 are
  currently identified (`act_set 0x09AC8690`, the executor `0x09AC5228`, the per-slot applier
  `0x09AC5520`). Mapping that outbound surface is a bigger job than the nine inbound slots.
- ⚠️ Nothing here is verified live. It is all static, from files, and internally cross-checked
  (§3) — but no custom overlay has been built or loaded.

**What it does NOT change:** the ~2 damaging-big-monster cap is architectural and closed
(`monster-ai`); this is about one monster's behaviour, not about how many can exist.

## 9. Tooling

```bash
tools/em_abi.py inventory --em     # 17 species overlays; one load VA, zero ctors
tools/em_abi.py vtables            # the 17 entity vtables, attributed 1:1
tools/em_abi.py interface          # 61-slot diff -> mandatory / optional / never
tools/em_abi.py classes em75       # the classes an overlay installs itself
tools/em_abi.py callers 29         # engine dispatch sites for a slot, in context
```

⚠️ `callers` matches on the vtable byte offset alone, which many unrelated classes share. High
counts (slot 25 → 284, slot 32 → 669) are contaminated; read the low-count sites in context and
treat the totals as an upper bound.

All of it reads `workspace/extracted/` only — no emulator, no cold boot.
