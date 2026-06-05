# Quest Loading Lifecycle (MHFU EU, ULES01213)

Reverse-engineered 2026-05-29 (Sections 31a–31j) while building the "inject a
non-native big monster into an existing quest" framework primitive. This is the
authoritative map of **how a quest's data + monsters get loaded**, the data
structures involved, the native engine functions to reuse, and the **hook points
that should become subscribable framework events**.

> Companion RE: `docs/agent_session_log.md` §31. Live facts: `docs/agent_memory_map.md`.
> Memory: `quest-singleton-monster-list`, `objmanager-eu-layout`.

---

## 1. The lifecycle (chronological)

| Phase | Where | What happens | Existing / proposed event |
|-------|-------|--------------|---------------------------|
| **A. Quest SELECTION / browse** | Elder, in village (`screen_state=0x71` quest-select) | Browsing quests loads a lightweight **map preview** (the area/map cell the HUD reads). **Does NOT populate `0x08A5C440` and does NOT load the monster model** — verified live: at the selector `0x08A5C440+4` is `0` (empty). (An earlier savestate that *looked* like selection-loads-model was contaminated with stale data from a prior quest run.) | proposed `on_quest_browsed` |
| **B. ACCEPT → LOADING SCREEN** | confirm → loading screen (`scr` 0x71→0xCC→0xC0→**0x20**) | **Everything heavy happens here, in a ~3 s window:** (1) the `.mib` parses into `0x08A5C440` ("2NDG"); (2) quest INIT (`0x08869904`) runs — sets `Quest+0x50`/`0x718`/`0x71c`, `clearTargets`, **`buildTargets`** → `targets[2]`; (3) **THEN the monster MODELS load** (RM tag-0x14 arena grows 0→~3 MB) by reading list A. Order verified by `tools/monitor_quest_load.py`: 2NDG + `t0cnt` appear, *then* models grow. | wraps `mhfu_on_quest_beginning` (`0x088655E4`) |
| **C. ENTER** | basecamp `area_index=98`, `scr=0x11` | Quest context active; player in basecamp. | `mhfu_on_quest_entered` (`0x0884CDFC`) |
| **D. SECTION / SPAWN** | field sections (`area_index=99…`) | The enemy/area system reads `targets[]` and **spawns** each monster at its record coords. | `mhfu_on_map_section_entered`, `mhfu_on_monster_spawned` |

**Critical ordering fact (CORRECTED Section 31k):** the monster MODEL loads in the
**LOADING SCREEN, AFTER `buildTargets`** — *not* at selection. So editing list A
at the `buildTargets` hook (Phase B) **IS early enough**: the model-load reads the
list afterward and loads the injected monster's model natively. Verified: adding a
Tigrex node at the begin hook → a 478 KiB em13 (Tigrex) model loaded into the
Giadrome quest's arena. (The earlier crashes were NOT "model too late" — they were
(a) no provisioning slot [fixed by the list-A node] and (b) an imperfect cloned
record [fixed by species-correct fields]. The "load at selection" theory was a
detour from contaminated savestate data.)

---

## 2. Data structures

### Quest singleton
- `Quest::objectPtr` (static) = **`0x08A62C7C`** → Quest instance = **`0x09A05DC0`** (fixed in EU).
- Layout (`tools/mhfu_external/mhp2g-decomp/include/quest.hpp`): `u8 pad_0x0[0x768];
  QuestTarget targets[2]; u8 pad[0x8A8-0x7C0];` — size **0x8A8**.
- Key fields set during Phase C init:
  - `Quest+0x10` = quest_timer (`0x09A05DD0`, u32 frames @30 Hz).
  - `Quest+0x50` = rec_base pointer (= `0x08A5C440`).
  - `Quest+0x71c` = **list A** pointer (big monsters); `Quest+0x718` = **list B** (small).
  - `Quest+0x768` = `targets[2]`, stride **0x2C**.

### Parsed-quest staging buffer (rec_base) = `0x08A5C440` (fixed EBOOT region)
Holds the decrypted+parsed `.mib`. Header:
- `+0x00` u32 offset; `+0x04` = **"2NDG"** magic (`0x47444E32`) = MHP2nd-G quest data.
- `+0x14` u32 → **list A** offset (big monsters); `+0x18` → **list B** offset (small).
- (other `+0x0c/+0x1c/…` → reward/objective/spawn sub-sections.)
- Used data ends ~`+0x1B84`; the tail is free scratch.

### List A node (big-monster list, stride 0x10)
- `+0x00` flag (1); `+0x04` 0; **`+0x08` mon-header offset (0 = END)**; **`+0x0C` record offset**.
- record / mon-header live at `rec_base + offset`.

### Monster record (`rec_base + node[+0x0C]`)
| off | meaning | Giadrome | Tigrex (native) |
|-----|---------|----------|------|
| +0x00 | emId (u16) | 0x4D | 0x4B |
| +0x05 | **model/class idx** | 02 | **04** |
| +0x08 | HP% | 0x64 | 0x6D |
| +0x1C | id/hash (u16) | 0x8D11 | 0xF692 |
| +0x20 | **spawn X** (f32) | 8957 | 9548 |
| +0x28 | **spawn Z** (f32) | 8755 | 6834 |
| +0x30.. | flags / params | 64 00 68 01 | 64 00 60 09 |

The **spawn POSITION is the record coords directly** (verified: a monster spawns
at exactly its `+0x20/+0x28`). em-type IDs = the entity `+0x1E8` type byte
namespace (Popo 0x46, Anteka 0x45, **Tigrex 0x4B**, Giadrome 0x4D).
**Cloning a record across species is imperfect** — at minimum `+0x05`, `+0x08`,
`+0x1C`, `+0x32` are species-specific; use the target species' native record.

### mon-header (`rec_base + node[+0x08]`)
`{ u16 emId; u16 0; u8 0xFF × 12 }`.

### QuestTarget (`Quest+0x768 + idx*0x2C`)
- `definitions[5]` (ptrs) @+0x00; `emId[]` @+0x14 (count-indexed ×4); `bytes_0x19[]`
  @+0x19; `count_0x1e` @+0x1E.
- **A target is a GROUP** holding up to 5 monsters (`count`). `buildTargets`'
  iterator always calls `addTarget(Quest, 0, …)` (index 0) → **all list-A monsters
  land in group 0**. The spawner spawns `definitions[0]` of each group, so a
  *second simultaneous* big monster needs its **own** group (`target[1]`).

### Model arena
- ResourceManager slab; RM index @ `0x0996FAC0` (12-byte `{tag,addr,size}`),
  **tag `0x14` = monster model**. Loaded in **Phase A**.

---

## 3. Native engine functions (reuse, don't reimplement)

| Function | EU addr | Signature / role |
|----------|---------|------------------|
| quest init | `0x08869904` | vtable/indirect; orchestrates Phase C |
| `clearTargets(Quest)` | `0x0886A444` | zero both target groups |
| **`addTarget(Quest, idx, record)`** | `0x0886A4CC` | append: `definitions[count]=record; emId=record[0]; count++` |
| **`buildTargets(Quest)`** | `0x0886D4D8` | iterate list A + B → addTarget; JAL site `0x08869EEC` |
| list-A iterator | `0x0886DB0C` | pass 1 (big monsters) |
| list-B iterator | `0x0886DC68` | pass 2 (small monsters; 0x3C-byte records) |
| model loader (raw PAC) | `0x088DDCF4` | `(descriptor, dest, src)`; `RM.alloc(0x14,size)` + load. Loads RAW file; full unpack/VRAM is higher-level. |

---

## 4. Injection results so far (Tigrex → Giadrome quest)

- **List-A node + target-split at Phase C works for DECLARATION + SPAWN intent**
  (no crash with provisioning), but the **model isn't loaded** (Phase A already
  ran) → spawning the 2nd monster crashes. (Crashes 11/13/14 all = no model.)
- **REPLACE** (retag the primary monster) crashes during load (Tigrex resources
  vs the Giadrome-sized reservation made in Phase A).
- **Conclusion / current target:** inject the node into `0x08A5C440` list A in
  **Phase A (selection)**, before the model-load — with a *correct* Tigrex record
  (template from the native one). Reliable mechanism = **hook the selection-parse
  function** that writes `0x08A5C440` and triggers the model-load (RE in progress).

---

## 5. Proposed subscribable framework events

Map each lifecycle phase to an event + an injection seam:

- `on_quest_selected(quest_id, map_id)` — fire after Phase A parse. **Pre-model-load
  injection seam**: edit `0x08A5C440` list A here to add/replace monsters so the
  engine loads their models natively.
- `on_quest_resources_loaded()` — after Phase A model-load completes.
- `on_quest_beginning()` — Phase C (exists: `0x088655E4`). Seam for target/spawn
  tweaks that don't need a new model (REPLACE, size, count within loaded models).
- `on_quest_entered()` — Phase D (exists: `0x0884CDFC`).
- `on_map_section_entered()` / `on_monster_spawned()` — Phase E (exist). Seam for
  per-entity tweaks (calm, resize, AI).

For a modding framework, the begin-hook (`buildTargets`) seam is the key new
capability for editing WHICH monster + loading its model. See §6 for the limit on
COUNT.

---

## 6. The big-monster spawn CAP (the remaining wall, Section 31L)

**Verified:** you can inject a monster's data + get its model natively loaded into
any quest (the begin-hook list edit lands before the loading-screen model-load —
a 478 KiB em13 Tigrex model loaded into the Giadrome quest). But **spawning a
2nd big monster crashes** (`0x300000000`), independent of spawn coords, with the
model loaded + record species-correct + the Giadrome spawning first.

**Why:** each quest provisions fixed-size big-monster structures (spawn / tracking
slots) sized for its *declared* big-monster count (1 for the Giadrome quest), in
the loading screen, around/before `buildTargets`. The model arena is sized *after*
the begin hook (so an injected model loads), but the spawn-slot array is sized
for 1 → the 2nd monster indexes out of bounds → wild pointer.

**What the cap is NOT:** not a coord issue; not at the elder selection (that's a
display-only icon catalog — no `.mib`, no model there); not a clean "count = 1 vs
0" cell (the 0-big vs 1-big RAM diff is too noisy; the spawn-block `0x09A0B610`
header's 16/0 fields are spawn-point capacity, not the big-monster count). It's a
**provisioned array** sized at allocation.

**Finding/raising it = future work needing better tooling** (PPSSPP "Fast Memory"
off still hard-crashes on macOS-ARM instead of logging the faulting PC): a PPSSPP
build that traps invalid PSP accesses to capture the faulting PC → the exact
array; OR decrypt the EBOOT + relocate the decomp to RE the enemy/spawn
provisioning; OR diff a native 2-big-monster snow quest (1-vs-2, clean) if one
becomes available. → proposed events `on_quest_resources_provisioned` /
`mhfu_set_big_monster_cap(n)`.

**Achievable now — REPLACE (not ADD):** retag the quest's existing big monster →
Tigrex (`g_qinj_replace=1`: begin hook rewrites list A node0's record + mon-header
to emId 0x4B + species fields, keeps native coords). Count stays 1 → no cap → the
engine loads + spawns a Tigrex instead of the Giadrome. The "add any monster to
any quest" framework primitive works for **swapping**; **adding** a 2nd big
monster is gated by the per-quest cap above.

---

## 7. Forcing a big monster to spawn in SECTION 1 — also blocked (Section 31N)

Tried (the "last PRX test"): keep REPLACE (1 monster, no cap) but rewrite the
record's spawn coords (`rec0+0x20`=X / `rec0+0x28`=Z, via `g_qinj_force_coords`)
to section-1 (15940/10536) so the engine *natively* spawns the Tigrex there.

**Result: no crash, but the Tigrex is INVISIBLE + INERT.** It lands in section 1
as a *logical object only*:
- present in the registry + the **tick/update chain** (`+0x1C4`/`+0x1C8`), frame
  counter `+0x092` advances → it's alive/ticking; position is settable (it spawns
  at Y=1000 floating — set `+0x204` and render `+0x44` to 0 to ground it);
- but its **Draw pointer `+0x008` stays `0x00000000`** (a visible Popo has
  `+0x008`=0x090B3440) → the renderer never draws it;
- and its **AI state `+0x334` stays 0 even fully untamed** (touch_entity + calm
  off, state reset) → no movement, no aggro. The section-1 enemy subsystem never
  attaches AI to it.

**Why:** section 1 has **no big-monster provisioning** — no spawn tile, no draw
slot, no AI-attach for a big monster. Forcing the coords places the entity there
but nothing wires it into section 1's render/AI systems. Same root as crash15
("a big monster can't be initially spawned in section 1"), now *non-fatal* because
REPLACE keeps the count at 1 (no cap overflow) — so it sits invisible instead of
crashing. A live *teleport* of the boss into section 1 fails identically
(`+0x008`=0). `g_qinj_force_coords` now defaults **0** (native coords → the Tigrex
spawns + RENDERS + behaves in its sections 6/7/8).

**To actually render a big monster in section 1 = future RE** (cap-class):
splice the entity into section 1's Draw chain (`+0x008`) + attach the section
enemy/AI subsystem, OR find where the engine provisions big-monster draw/spawn
slots per-section and extend section 1. Same tooling gap as the spawn cap (§6).

---

# Big-monster injection / section-1 / aggro / render-bug detail (migrated from CLAUDE.md, 2026-06-05)

**Quest monster-injection status (Section 31, 2026-05-29):** VERIFIED — a PRX
begin-hook on `buildTargets` (`jal` @ `0x08869EEC`, gated install at TITLE/MENU
per Section 26) can edit the quest's big-monster list (list A in `0x08A5C440`)
before the loading-screen model-load, so the engine **natively loads a
non-resident monster's model** (a 478 KiB em13 Tigrex model loaded into the
Giadrome quest). **SWAPPING works** (`g_qinj_replace`: retag list-A node0's
record+mon-header → Tigrex, emId 0x4B + species fields +0x05=0x04/+0x08=0x6D/
+0x1C=0xF692/+0x32=0x0960; 1 big monster, native coords). **ADDING a 2nd big
monster is BLOCKED by a per-quest big-monster SPAWN CAP** — the quest provisions
fixed spawn/tracking slots for its declared count (1) at the loading screen, so
the 2nd monster's spawn indexes OOB → `0x300000000`. The cap is a provisioned
array (not a count cell / not at the elder selection / not coord-related); raising
it needs better tooling (PPSSPP that traps invalid accesses, or EBOOT-decrypt RE)
— deferred. Also: a big monster CANNOT be initially spawned in section 1 (no
big-monster spawn tile there at quest-begin; crash15) — they spawn in 6/7/8 and
roam. **Section 31N (2026-05-29): forcing the REPLACE'd Tigrex's record coords to
section 1 (`g_qinj_force_coords`) is NON-FATAL under REPLACE (1 monster, no cap),
but the Tigrex is INVISIBLE + INERT there** — it lands as a logical object only
(in the tick chain, frame counter advances, position settable) and AI state
`+0x334` stays 0 even fully untamed (no movement/aggro): section 1 has no
big-monster provisioning (spawn tile + AI attach). So `g_qinj_force_coords`
defaults **0** → native coords → the Tigrex spawns + RENDERS + behaves in 6/7/8.
**SOLVED (Section 33, 2026-06-04): a big monster IS fully workable in section 1 —
spawn-native-then-RELOCATE, not force-coords.** Let him spawn at native coords
(section 6 → full provisioning: model + AI attach + draw), then in-area TELEPORT
his world pos `+0x200` into section 1 and apply the `+0x29A` visibility fix. The
relocate carries his already-attached AI; verified live the teleported Tigrex is
**visible AND aggressive** in section 1. The old `g_qinj_force_coords` path landed
him invisible+inert only because forcing the spawn RECORD coords meant the engine
never AI-attached him at load. Reference mod:
`framework/prx/mods/lua_host/scripts/tigrex_section1.lua`. Full detail + proposed subscribable events:
`docs/QUEST_LOADING_LIFECYCLE.md` §7. Live mechanism:
`framework/prx/mods/tigrex_inject/mod.cpp` (descriptor mod). Re-verified
2026-05-30 on the typed API end-to-end (cold boot → replace at native coords →
Tigrex visibly roams snow sections 6/7/8; section 6 = area index 100).

**How far a relocated big monster reaches — limits map (Section 33b, 2026-06-04):**
What a big monster needs to FUNCTION in a zone, and where each gate sits:
render ✅ (`+0x29A` section match, see `docs/agent_memory_map.md`) · tick/AI-attach
✅ (carried by spawn-native-then-relocate) · floor/collision + locomotion ·
autonomous aggro. Findings:
- **Native big-mon sections (snow 1/6/7/8):** all gates pass — visible, ticks,
  roams, chases, attacks.
- **NON-native FIELD sections (snow sec4=area92, sec5=area93):** loads + draws +
  ticks + minimap, BUT no floor/navmesh → the **motion integrators** `0x09AC8D9C`/
  `0x09AC8E80` (`pos += VFPU velocity`, NOT a hard reset) drive him to a fixed
  **fallback origin ~5040,5005** and he FALLS / goes inert. Position writes place
  but can't fabricate floor/nav. Area numbering is NOT sequential (sec1=99, sec6=100,
  sec5=93, sec4=92, basecamp=98) — gate "field section" on `screen_state==17 &&
  area!=98`, not `area>=99`.
- **Basecamp (area 98):** SURPRISE — it HAS working floor + locomotion + animation
  for him (teleported Tigrex's Y settled to ground ~1133). He renders, ticks, lands,
  animates, enters aggro STANCE on command — the ONLY missing piece is autonomous
  aggro targeting.
- **Aggro / "target the player" — ★ ENGAGE-SETTER PINNED (Section 33g, 2026-06-05),
  memory `big-monster-aggro-target`:** the engage flag `+0x05DC=1.0` is one float
  inside a **quad `sv.q` (PC `0x09AC6084`) in the per-frame aggro-commit fn
  `0x09AC6040`** — it writes `monster+0x5D0..+0x5DF` (pursuit vec3 at +0x5D0 + engage
  at +0x5DC) in ONE op, every frame while engaged. The fn resolves the target via
  `z_un_088dcebc([0x09A4F0C4])` (the player), gates on `target+0x2A4==0` AND
  **`monster+0x29A == target+0x29A` (section match)**, loads the `1.0` literal at
  `0x09AC6090`. **A 4-byte mem-BP at `+0x5DC` never fires** (a quad `sv.q`'s memcheck
  checks the quad BASE `+0x5D0`, not `+0x5DC`) — this is why the whole "engage-write is
  uncatchable / FPU-blind" saga happened. ⚠️ **The "FPU-blind wall" (Section 33d below)
  was a MISDIAGNOSIS** — PPSSPP float/VFPU mem-BPs DO fire; the engage write just wasn't
  a normal addressable store. **To FORCE aggro externally** (e.g. basecamp, where the
  target resolver returns null so the engine's `sv.q` gates fail and won't overwrite):
  write the `+0x5D0` pursuit vec3 (toward the player) + `+0x5DC=1.0` together (+ optional
  `+0x0334 ai_state=2`). **How it was pinned (reusable):** the PPSSPP fork
  (`tools/ppsspp-fork/`) + force `CPUCore=2` (IR_INTERPRETER — enum trap: 0=interp,
  1=JIT, 2=IR_INTERP, 3=IR_JIT) + a one-line `IROp::Store*` watch in `IRInterpreter.cpp`
  (`docs/agent_debugging.md`) catches any "value changes but no mem-BP fires" write. The
  fork also adds `savestate.save/load` debugger commands (~3s scenario reloads). Below =
  the SUPERSEDED §33c/§33d decode (kept for the real address/disasm notes — singleton
  `0x089CC438`, target resolver `z_un_088dcebc([0x09A4F0C4])`, section-match gating):
- **[SUPERSEDED §33c] aggro-eval fn `0x09A66098`**`(monster,a1)`
  runs per-tick and was believed to ACQUIRE (`monster+0x2A4=1`) iff:
  **A** `monster+0x29A == player.area` (`[player]+0x6AF0E` == `get_area_index()`);
  **B** `monster+0x1E6 == [player]+0x28` (player section byte);
  **C** combat-enable `[0x09A44C5C]→obj; obj+0x5C(u8) != 0`;
  **D** range/target via `0x09A631A8` (target = `z_un_088dcebc([0x09A4F0C4])`,
  pos `target+0x200`; needs `[player]+0x6AF14 & 1`). **Player master singleton =
  `0x089CC438`**; player-pos base = `0x089CBBE4` (`+0x20` = `0x09998D50`). Aggro
  gating is **section-equality + a combat-enable global + a target resolver**, NOT
  sight-cone-first; the det-radius `0x09BC1030` is a secondary path. Forcing the
  signature alone = stance (skips ACQUIRE) → no pursuit. **METHOD WALL:** PPSSPP
  mem-BP is blind to ALL FPU `lwc1`/`swc1` (why engage-write + pos-read were
  "invisible") → cracked by static disasm after early-armed INTEGER ptr-global
  read-BPs caught the brain loading the player singleton (`src/ppsspp_debug/
  re_aggro_ptrhunt.py`). **Basecamp-aggro RECIPE (all integer, JIT-immune; UNTESTED,
  rig ready):** force A `+0x29A=area`, B `+0x1E6=[player]+0x28`, C `[0x09A44C5C]
  +0x5C=1`, D-bit `[player]+0x6AF14|=1` → eval should ACQUIRE. Test mod
  `framework/prx/mods/lua_host/scripts/tigrex_aggro.lua` (cold boot only; logs each
  gate to show which still blocks — likely D, the target resolver returning null
  in a non-combat zone). **RETARGET-to-monster mod:** redirect `z_un_088dcebc
  ([0x09A4F0C4])` to a monster object → tigrex hunts the monster. Puppet-attack via
  `on_bigmonster_action` remains the no-RE shortcut.

**SWAP RENDER BUG SOLVED + FIXED (Section 33, 2026-06-04).** The separate symptom
— a Giadrome→Tigrex swap (native coords) being **invisible on first entry to his
section until he roams once** — is now fully RE'd and fixed. Root cause: the swap
never initialises the monster's **section tracker `entity+0x29A`** (the u16 map
section he's considered to be in, same encoding as player `area_index`). The
per-frame big-mon **visibility gate `0x09AC4960`** renders him only if `+0x29A ==
player section` AND `+0x638 & 0x8000`; with `+0x29A` wrong he's culled until his
first roam (a real section transition) sets it. **`entity+0x008` was a red
herring** — it's the per-frame *frustum-cull* flag (toggles `0↔0x090B3440` every
frame), NOT a draw-registration gate; the drawnode + ObjManager draw-list are
already valid pre-roam. **Fix** (`framework/prx/mods/lua_host/scripts/tigrex_spin.lua`,
hot-reloadable): while the player is physically co-located with the Tigrex, force
`+0x29A = get_area_index()` (+ OR `+0x638 |= 0x8000`) → the engine clears the
skip-draw bit itself, coherent, no race. Verified live (write wrong section →
vanish + map marker moves; write player section → reappear). Cells + gate fn:
`docs/agent_memory_map.md` (`+0x004`/`+0x008`/`+0x29A`/`+0x638`, fn `0x09AC4960`);
memory `giadrome-tigrex-render-bug`. (NOTE: this supersedes the old
`snow-map-section6-and-draw-interp` "engine binds Draw on section entry / `+0x008`
== render membership" reading — the real gate is the `+0x29A` section match.)
