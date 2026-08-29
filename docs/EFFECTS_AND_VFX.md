# Monster visual effects in MHFU — and what "porting effects" can actually mean

**Status 2026-08-29.** The static half is complete and reproducible: `tools/em_effects.py`
recovers any species' effect vocabulary offline, and the whole spawn chain is mapped (§2, §8).
The live half is **half done** — the trigger primitive ships and the call runs without taking the
game down from an override callback, but every spawn so far returned a zero handle, so *nothing
has been seen on screen yet*. Read §5 for which context is safe and §6 for what is still missing.
Live driver: `tools/port_effect_probe.py` +
`framework/prx/mods/lua_host/scripts/zinogre_fx.lua`.

---

## 1. The finding that shapes everything else: there is no effect DATA to port

A ported monster inherits the host's visual effects, and the reason is architectural rather
than a gap in the porter.

- **Effects are emitted from CODE, not from a table.** The species overlay runs
  `switch(entity+0x298) -> switch(entity+0x299)` into per-action MIPS, and that code calls the
  spawn with the effect id, the bone and the frame as **literal immediates**
  (`AI_SCRIPTING_ENGINE.md` §33). There is no per-action effect record to read, edit or inject.
- **The MHP3rd monster group contains no effect file.** A group is `[overlay copies]` followed
  by one or more `[model+skel PAC][GE geometry][moveset .anim]` triples — the overlay count is 2
  or 4, and a family shares one overlay (em001 carries Rathian *and* Rathalos). The Zinogre's is
  `file_05337..05341`: `em040m0.ovl`, `em040m1.ovl`, and exactly one asset triple. Every file
  accounted for, none of them a particle definition. `file_05342`/`05343` are **not** his — they
  are `em041m0`/`em041m1`, which is why they read as an unexplained identical-size couple.
  `tools/mhp3rd/em_groups.py` prints all 43 groups.
- **MHFU's per-species resource table is SOUND.** `0x09BC8400` is a `u16[3]` per species that
  the quest loader hands to `0x088DDBD8(loader, slot, file_id, 0)` at `0x088DE260`. Tigrex (75)
  gets `6345/6346/6347` — a ~400 KB raw sample blob, a `dbsT` bank and a `Head ` table. Audio,
  not VFX.
- **The monster model PAC has no effect sub-resource** either: `[skel, pmo, tmh, anim, skel2,
  pmo2, tmh2]`, where subs 4-6 are a small ~6-bone secondary model.

So "port the Zinogre's lightning" is not an operation that exists in the shape the model and
animation ports had. What exists is **MHFU's own effect library plus an entry point that takes
an id and a bone** — and a mod that can call it at a frame of its choosing gets visuals without
porting anything.

⚠️ Where the effect DEFINITIONS live is still open (§7). Nothing above depends on it: it decides
whether *new* effects can be authored, not whether existing ones can be fired.

---

## 2. The spawn chain, end to end

```
em<N>.ovl   spawn_effect(entity, effect_id, bone)              0x09D36B58 in em75
  -> game_task 0x09ACB3E0(entity, id, base_species, owner, bone, &pos, mode)
       -> EBOOT 0x08883B54(effect_mgr, ent[0x5F6], id, base_species, owner, bone, &pos, mode, …)
            -> 0x088838B0 -> 0x0887FFE8 -> 0x0889EADC -> 0x0889ECB8   (descriptor build)
```

`0x09D36B58` is a thin wrapper and is worth reading as the specification of a correct call:

| step | what it does |
|---|---|
| `0x0886421C(entity+0x80, bone)` | `*(u32*)(entity+0x190) + bone*0x250` — the joint record |
| `+0x100/+0x104/+0x108` of that | the bone's **world xyz**, copied to the stack |
| `0x08866284(entity)` | owner tag: `class(entity[0x1E9]) \| entity[0x1E4]` |
| `*(s8*)(0x089AA1C8 + entity[0x1E8])` | species → **base species** |
| `0x09ACB3E0(…, mode=3)` | the spawn |

**Generic addresses only** — `0x09ACB3E0` is in game_task and the rest are EBOOT, both resident
for every species. That is what makes the framework's own implementation species-independent.

### The four entry points a species overlay can use

All of them end in the same `jal 0x09ACB3E0`. Argument positions read off the disassembly:

| entry | signature | note |
|---|---|---|
| `0x09ACB3E0` | `(ent, id, base_species, owner, bone, &pos, mode)` | the spawn itself |
| `0x09ACB9C8` | `(ent, frame_lo, frame_hi, id, bone, mode, slot)` | **frame-gated** |
| `0x09ACB610` | `(ent, id, bone_base, bone_off)` | positional; mode comes from a second per-species byte table at `0x089AB79C` |
| overlay-local | `(ent, id, bone)` | e.g. em75's `0x09D36B58` |

`0x09ACB9C8` is the interesting one. It fuses `anim_cursor_reached` and the spawn into one call,
so it **is** the "at frame F play effect E at bone B" primitive — 67 call sites across 9 species.
A scripted moveset that wants authentic timing is reproducing this shape.

### 🔴 The section gate — a silent no-op, not an error

`0x09ACB3E0` first calls `0x08865BAC(entity)`, which is

```
entity[0x29A] (u16, the section the monster has committed to) == <global current section>
```

If it fails, the spawn is **dropped and the call returns normally**. An effect asked for by a
monster the player cannot see never appears and reports nothing. A zero handle therefore means
"not spawned", which is not the same as "bad effect id" — tell them apart before blaming the id.

---

## 3. The id space is shared, and biased for exactly seven species

`0x09ACB3E0` adds a per-species constant to the id before the EBOOT spawn, keyed on
`entity+0x1E8` (switch at `0x09ACB450`):

| species | bias |
|---|---|
| 80 | +30 |
| 13, 16, 30, 35, 61, 62 | +100 |
| everything else | 0 |

Every other species indexes the same numbers. 59 distinct ids appear across the 17 overlays,
and the common ones are common: `87` in **nine**, and `2`, `38` and `74` in five each. The spawn also receives `base_species` as a **parameter** (it ends
up in the 60-byte descriptor built at `0x0889ECB8`), so whether an id means the same picture
across species is decided downstream and is not settled here — see §7.

`0x089AA1C8` is a `s8[]` mapping species → base species (subspecies collapse onto their parent:
`[36]=2`, `[77]=16`, `[81]=75`, …). Note it is **`0x089AA1C8`, not `0x089BA1C8`** — the older
address in `agent_memory_map.md` was a transcription slip and points into unrelated code.

---

## 4. What each species actually emits

`tools/em_effects.py --census` (all 17 overlays, 4 entry points, literal arguments only):

```
em01  11 ids: 2 4 16 32 38 50 78 87 92 114 160
em02   3 ids: 10 45 85
em07  14 ids: 31 34 35 38 68 69 70 71 72 73 75 76 77 78
em14  11 ids: 2 4 34 36 44 62 74 84 87 91 110
em15   4 ids: 38 89 91 99
em17   2 ids: 81 87
em20   9 ids: 2 32 38 56 60 74 87 104 160
em21   3 ids: 27 30 87
em33   0 ids
em40   5 ids: 2 56 74 87 104
em54   7 ids: 31 36 60 62 73 74 80
em55   3 ids: 20 68 69
em58   2 ids: 68 69
em59   0 ids
em75  28 ids: 10 11 14 24 28 29 36 37 39 40 42 43 55 56 59 60 61 62 66 70 71 72 79 81 84 85 87 100
em82   7 ids: 2 32 38 74 87 92 160
em83   3 ids: 27 30 87
```

Per-species detail carries the bone and, for the frame-gated form, the frame:

```
tools/em_effects.py file_06108.bin
  handler 0x09D42C48   7 sites  14@b49@f198 37@b33@f116 39@b33@f18 40@b33@f206
                                42@b33@f4 72@b33@f128 81@b33@f216
```

That is a readable move: *at frame 4 effect 42 at bone 33, at frame 128 effect 72, at frame 206
effect 40* — the exact recipe a scripted port would reproduce with its own bone numbers.

⚠️ **em## is the species id, but this repo has NOT confirmed which monster each one is.**
`MON_TIGREX = 0x4B = 75` and `MON_GIADROME = 0x4D = 77` are verified in the framework;
the table in `MEMORY_STRUCTURES.md` §"Monster Type IDs" disagrees with both and should not be
trusted. em07's vocabulary (31, 34, 35, 68-78, almost all its own) is what an element-heavy
monster looks like, but that is circumstantial.

⚠️ **Bone indices are the emitting species' own.** em75 puts 60 at bone 33 and 79/85/87 at bone
37 because those are the Tigrex's mouth and head. A port carries its own skeleton, so the same
numbers land somewhere else.

---

## 5. Firing one from a mod

```lua
mhfu.spawn_effect(entity, effect_id, bone)   -- returns a handle; 0 = not spawned
mhfu.bone_pos(entity, bone)                  -- x, y, z of that bone, world frame
```

🔴 **CALL `spawn_effect` ONLY FROM INSIDE AN OVERRIDE CALLBACK** — `on_bigmonster_action` and
friends, which run on the exec thread with the game thread blocked. It allocates out of the
effect manager, so the free-running 2 Hz `mhfu_tick` worker races the engine, same rule as
`resolve_attack`.

**And there is no per-frame driver — though not for the reason the first takes suggested.** The
obvious design is a native `ai_step` prefix firing a spawn every N frames. One cold boot ran a
three-rung ladder on it:

| rung | what | result |
|---|---|---|
| A | `spawn_effect` from an `on_bigmonster_action` callback | 2 calls, game fine |
| B | the ai_step driver **dry** — everything but the engine call | `attempts=2 returns=2`, fine |
| C | the same driver, live, budget 3 | armed, then the log stops, for good |

B clears our own code on that path — the counters, the ctx deref, the bone read all behave — and
A shows the call itself is survivable in an override callback.

🔴 **C IS NOT ESTABLISHED, AND THE RETRACTION MATTERS MORE THAN THE CLAIM.** Two takes that armed
the live driver ended within seconds of arming, which looked conclusive. Then **three later takes
ended at 250-310 s having made no effect call at all** — `input.analog.send timed out` in one, a
plain read timeout in another. The emulator container had been up two days on a host with 4 GB
free, and takes were dying on their own. The base rate of a take ending in that window is high,
so the association between "armed the live call" and "the take ended" does not survive.

What stands: the call works from an override callback, and the per-frame driver is **withheld
rather than condemned** — not shipped because nobody has shown it safe, not because it has been
shown fatal. Re-testing it wants a freshly restarted emulator and a counter in scratch RAM read
back afterwards, so the verdict does not depend on the debugger link surviving.

**What a moveset actually wants** is the engine's own
`0x09ACB9C8(ent, frame_lo, frame_hi, id, bone, mode, slot)`, which fuses the animation-cursor
test with the spawn — "at frame 206, effect 40 at bone 33" in one call. Reaching it safely means
a call site the engine already reaches it from: **wrap an em-overlay vtable slot**
(`EM_OVERLAY_ABI.md` §10), do not prefix the AI tick.

---

## 6. The four things the Zinogre needs, and where each stands

| want | mechanism | state |
|---|---|---|
| lightning at a bone on a frame | `spawn_effect` from an override callback + the id census | primitive shipped; which id looks like what is the live sweep |
| effects that follow him | the spawn takes a bone and reads its live world position each call, and the bone index is a descriptor field | plausible by construction, unverified |
| "more glowing when engaged and furious" | rage is `entity+0x638 & 0x20` (set in em75's `0x09D32490`, read at `0x09D2C614`); a brain can poll it and drive a persistent effect | no engine-side visual to reuse yet |
| electric orbs he throws | a **projectile**, not an effect — the MHP2G decomp names `Singleton<ShellManager>` alongside `EffectManager`, so the engine has a separate shell system | **not located in EU.** Its own RE |
| the bugs that orbit him | neither an effect nor a shell in MHFU's vocabulary — nothing native orbits a monster | open. Candidates: repeated spawns at offset positions, or entity clones (`entity_clone`) |

Rage is a plain flag, so a scripted monster can gate its own visuals on it without any new RE.
What rage does *natively* to a monster's appearance is not yet mapped.

⚠️ **Do not assume the effect spawn covers the projectile.** It places a visual at a bone and
takes no velocity, no lifetime and no collision — three effect calls can make a flash at his
mouth, and none of them will produce something that flies and hits. The JP symbol
`objectPtr__25Singleton<12ShellManager> = 0x09C14250` says the system exists; EU addresses are
0 of 212 co-located with JP for these functions, so it has to be found by behaviour.

---

## 7. Open

1. **Where the effect definitions live.** The chain reaches `0x0889ECB8`, which builds a 60-byte
   descriptor from a flag word and hands it on inside an `0x0889E…-0x088A6…` library. The
   library was not followed to a resource. Until it is, we know effects can be *fired* but not
   whether new ones can be *authored*.
2. **Is an id species-scoped?** `base_species` is passed all the way into the descriptor. If the
   lookup is keyed on it, em07's id 70 and em75's id 70 are different pictures and the census is
   a per-species vocabulary rather than a shared one. The live sweep is the discriminator: ids
   outside em75's 28 either draw something or they do not.
3. **em## → monster name.** Needs a render pass over `file_0{species+6110}`. The table in
   `MEMORY_STRUCTURES.md` §"Monster Type IDs" contradicts the two ids the framework has verified
   and should be treated as wrong until re-derived.
4. **The shell/projectile system.** Named in the JP decomp, unlocated in EU. Everything the
   Zinogre throws needs it.
5. **Nothing has been seen on screen.** Every spawn so far returned a zero handle, and the two
   candidate explanations — the section gate, and the executor detour never installing (below)
   — have not been separated.
🔴 **AND THE SWEEP'S SEAM ITSELF IS UNRELIABLE — this is the concrete next lead.** Three takes
armed cleanly and fired **nothing**, and `framework.log` says why:

```
[ai] exec entry not original (0x6826c71e) — skip patch
[zfx] ARMED — 28 ids, one per action dispatch, bone 37
```

`on_bigmonster_action` is a **code-word patch on the executor entry**
(`install_overlay_action_hook`, `src/core/ai.cpp`), and it only lands when the bytes are the
original prologue. `0x6826c71e` is a **JIT block marker** — PPSSPP had already re-translated the
block, so the framework correctly refused to patch and the callback can never fire. It is
CLAUDE.md §1 biting the framework's own headline seam, and from Lua it is invisible: the
callback registers, returns success, and is simply never called.

So "0 of 28 ids accepted" in those takes is **not** a statement about the ids, the section gate,
or the monster's dispatch rate. It is a statement about the hook. Before reading any effect
result, grep the log for `exec entry not original`. The fix is to get the detour in earlier —
the deferred TITLE/MENU queue, as the ai_step prefix already does — rather than at overlay load.


---

## 8. Addresses added by this work

| what | address | notes |
|---|---|---|
| species-biased effect spawn | `0x09ACB3E0` | `(ent, id, base_species, owner, bone, &pos, mode)` |
| frame-gated spawn | `0x09ACB9C8` | `(ent, frame_lo, frame_hi, id, bone, mode, slot)` |
| positional spawn | `0x09ACB610` | `(ent, id, bone_base, bone_off)` |
| EBOOT spawn | `0x08883B54` | 8 register args (EABI32) + 3 stack |
| effect-manager singleton ptr | `0x08A62D8C` | second singleton at `0x08A62D84` |
| section gate | `0x08865BAC` | `entity[0x29A] == current section` |
| joint record for a bone | `0x0886421C(entity+0x80, bone)` | `= *(u32*)(entity+0x190) + bone*0x250` |
| **joint array base** | `entity+0x190` | ⚠️ corrects `+0x4C8`; world xyz at joint `+0x100` |
| owner tag | `0x08866284(entity)` | `class(ent[0x1E9]) \| ent[0x1E4]` |
| species → base species | `0x089AA1C8` (s8[]) | ⚠️ corrects `0x089BA1C8` |
| species → effect mode | `0x089AB79C` (s8[]) | used by `0x09ACB610` |
| per-species SOUND files | `0x09BC8400` (u16[3]) | loaded at `0x088DE260` |
| monster model PAC file id | engine `species + 6111` | extracted `file_{species+6110}` — the extractor's index is one LOWER than the engine's. Tigrex 75 → `file_06185` |
| resource loader | `0x088DDBD8(loader, slot, file_id, 0)` | |
| rage flag | `entity+0x638 & 0x20` | set in em75 `0x09D32490` |
