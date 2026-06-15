# Big-Monster Overlay Relocation — the generic multi-big-monster path

**Sections 39–47 (2026-06-09/10).** How to run **N big monsters of different families
simultaneously** in one quest. This supersedes the "forge" approach (manual
resource mount/register into a foreign quest) and the "cap = 2 groups" framing —
both were symptoms of the real constraint documented here.

**TL;DR status:** RAM wall **solved** (stock-PPSSPP, §6); relocator **built+validated**
(M1, §3); loader **built** (M2, §3b); native-monster rebind **proven on HW**; the
big-mon manager is a **TaskManager linked-list task** and the per-monster provisioning
**constructor is found** (`0x088a7090`, §4) — so **N-same-family** is buildable now
(clone+splice a manager, no species step) and **N-different-family** needs only the
remaining "how a manager learns its species" trace (§4, step 2). The relocation+RAM
work is **proven** but only redirects a *fully-set-up native* monster; the ADD path
needs the manager construction in §4.

**§48 (2026-06-10) — N-SAME-FAMILY SOLVED + verified in-game. See §7 + §10.** The old
"per-quest spawn cap" is **retracted** (`buildTargets 0x0886D4D8` is uncapped). The real
structure: native dual-big = **TWO target GROUPS** (`Quest+0x67C=2`, target[0]+target[1]) →
**two managers → two entities**; one group with count 2 → one (singleton) manager → only 1
spawn. The shipped **`bigmon_dup`** mod calls **`mhfu_quest_add_monster`** (append a list-A node
+ buildTargets-postfix split into group1 + `+0x67C=2`); the forge is irrelevant for same family.
**Verified live:** 2 working Tigrex (registry slot1 `0x090c1b00` + slot2 `0x090c9500`), 2nd alive
+ aggros + attacks. **N-different-family = this same split + a relocated 2nd overlay** (M1/M2).

Companion memory: `big-monster-cap-is-two` (full chronological detail).
Related: `docs/QUEST_LOADING_LIFECYCLE.md`, `docs/AI_SCRIPTING_ENGINE.md`,
`docs/agent_memory_map.md`, memory `taskbase-abi-eu`.

---

## 1. The real constraint: one em-overlay slot

Monsters split into two AI classes:

- **Small monsters** (popo, giadrome, anteka, …): AI lives in the **EBOOT**
  (`0x0886xxxx`), always resident, uses **no** overlay slot. Many coexist freely.
- **Big monsters** (the 17 with an `em*.ovl`): AI/script is a **position-dependent
  overlay** streamed from `DATA.BIN`. **Every `em*.ovl` loads to the SAME fixed
  slot** — so only one big-monster *family* can be resident at a time.

The 17 overlay monsters (`configure.py` in `tools/mhfu_external/mhp2g-decomp`):

```
em [1, 2, 7, 14, 15, 17, 20, 21, 33, 40, 54, 55, 58, 59, 75, 82, 83]   FIRST_ID 6043
```

Overlay-class load slots (from the decomp `OverlayInfo.load_address()`):

| class      | slot base    |
|------------|--------------|
| `em*`      | `0x09d15100` |
| `*_task`   | `0x09a5a580` |
| `*_sub`    | `0x09c14280` |
| `stage*`   | `0x09d5df80` |

This is why **REPLACE works** (reuse the one slot — `mhfu_quest_replace_monster`),
why **same-family / same-archetype multi works** (one overlay serves all entities;
scripts are shared by design — see the same-species probe in the memory file), and
why a **2nd different family natively crashes** (its script struct `[entity+0x4C8]`
is never streamed into the occupied slot).

> Tigrex = **em75**, on disk **raw/uncompressed** at
> `workspace/extracted/data_files/file_06108.bin`.
> (The `file_0604X` "ovl" label in older notes was wrong — those are the MODEL PACs:
> PMO/TMH/PMO2/HITS.)

## 2. The MWo3 overlay format

64-byte header, then `text`, then `data`; bss is zero-filled at load.

```
struct MWo3Header {           // struct.unpack '<4sIIIIIII32s'
    char     magic[4];        // "MWo3"
    uint32_t overlay_id;
    uint32_t load_address;    // VA of file offset 0 (e.g. em75 = 0x09d1a180)
    uint32_t text_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t static_init_start;   // ctor list (run after load)
    uint32_t static_init_end;
    char     name[32];        // e.g. "em75.ovl"
};  // expected file size == 64 + text_size + data_size
```

**Footprint subtlety:** the file image's `load_address` sits **above** the slot
base. For em75: slot base `0x09d15100`, file image at `0x09d1a180` → the region
`[0x09d15100, 0x09d1a180)` (0x5080 bytes) is the overlay's **lower BSS**. Overlay
code reaches into it (e.g. `lui v0,0x09d2; lw a0,-0x68a0(v0)` → `0x09d19760`), so
the **relocatable footprint is `[SLOT_BASE, load_address + file_size + bss_size)`**,
not `[load_address, …)`. File offset → VA is `load_address + offset`; code begins at
file `0x80` (64B header + 64B zero pad).

The overlay is **raw-loaded to its fixed VA with no runtime relocation** (the
addresses are baked at build time). The decomp's per-overlay reloc file
(`config/em/em75.reloc_addrs.txt`) is empty — splat auto-detects references; there
is no shipped reloc table.

## 3. M1 — the relocator (`tools/ovl_reloc.py`)

To place a 2nd family's overlay at a *different* VA, we relocate a copy ourselves.
The tool parses MWo3 and fixes every reference that lands in the overlay footprint:

- **MIPS_26** `j`/`jal` whose target is in-footprint → rewrite target.
- **HI16/LO16** `lui` + (`addiu`/`lw`/`sw`/…/`ori`) forming an in-footprint address
  → rewrite the pair. Pairing uses **register-write dataflow**: a `lui`-set register
  is a `%hi`-holder until it is **written**; `addiu R,R,lo` *consumes* it (R becomes
  a full pointer, later `off(R)` are struct offsets, **not** `%lo`). A naive
  "last-lui-per-reg" heuristic mis-pairs pointer arithmetic and corrupts code.
- **MIPS_32** data words whose value is in-footprint → `+= delta`.

**The delta MUST be 64KB-aligned** (`low16 == 0`). Then `%lo` immediates never
change and a shared `lui`'s `%hi` shifts uniformly, eliminating the "`%hi` diverges
when `%lo` straddles 0x8000" error class. `relocate()` raises on an unaligned delta.
M2 controls placement, so it always aligns.

Validation (`--selftest`): an independent dataflow extractor checks, for 5 aligned
deltas, that **every in-footprint reference moves by exactly `delta`, nothing
out-of-footprint is touched, and no reference leaks into the old footprint**. em75:
2476 MIPS_26 + 930 hi/lo lo-sites + 1471 MIPS_32 = 4877 refs, all correct.

```bash
python tools/ovl_reloc.py <em.ovl> --selftest
python tools/ovl_reloc.py workspace/extracted/data_files/file_06108.bin \
       --newbase 0x09700000 -o em75_reloc.ovl     # newbase: 64KB-aligned slot
```

> spimdisasm/splat could not be used as external ground truth: the decomp only
> partially analyzed em75 (no function boundaries → its symbol-finder didn't run),
> so the built-in dataflow cross-check is the validator.

## 3b. M2 — the runtime loader (`framework/prx/src/core/bigmon_overlay.cpp`)

`mhfu_bigmon_load_relocated(path)` opens an `em*.ovl` from memstick
(`ms0:/PSP/PLUGINS/mhfu_framework/overlays/em75.ovl`), sizes the footprint from the
MWo3 header, places + relocates + zeros bss, and returns the region/delta. The C
relocator (`src/core/ovl_reloc.cpp`) is **byte-identical to the Python** (host self-test).
Lua binding: `mhfu.load_relocated_overlay(path)`. Placement, in priority order:
1. **NATIVE slot** — if the em slot (`[0x09d1a180]==0`) is FREE (small-mon host), place
   at the native slot (delta=0); the added monster's native `+0x4C8` then resolves with
   no rebind.
2. **partition 2** (works if the allocator is resized — real-HW / fork).
3. **EXTRA RAM** — the stock-PPSSPP path (see §6): `[0x0A000000,0x0C000000)`, persistent.
4. **VOLATILE** — last resort, UNSAFE (reclaimed on streaming).
Idempotent (`g_placed`). HITL-verified: places em75 byte-correct in extra RAM and a
fully-set-up native Tigrex rebound to it (`entity+0x4C8 = struct + delta`) fights normally.

## 4. M3 — the per-monster setup is a TaskManager task (the real ADD seam)

Disassembled clean from `workspace/extracted/PSP_GAME/SYSDIR/BOOT.BIN` (ELF PH0
`vaddr 0x08804000`, file off `0x25b4`; live RAM is JIT-marker-polluted, do not disasm it).

**Why relocation alone isn't enough.** Relocation + `+0x4C8` rebind only redirects a
monster the engine **already fully set up**. An *added* monster has no engine setup, so
its overlay-init runs with a **NULL resource context** and crashes (`0x08859b54`,
`[NULL+0x19014]`). So the ADD needs the engine's **per-monster provisioning**.

**The provisioning, fully traced (HITL, native Tigrex load):**
```
TaskManager update 0x08895538(a0=[0x08A8B1E8] = TaskManager**, →0x08B0C6A4)
  walks a LINKED LIST: s0=[TaskManager]=head;  loop:
     state=[task+0x11];  if state!=1 -> vt[0xC](task)   ; per-frame DRIVE
                         if state==1 -> vt[8](task)      ; INIT / PROVISION (once)
     task = [task+0x18]                                  ; NEXT
  (driver 0x088971A0 -> overlay-init 0x09a5f768(a0=manager, phase) = 13-state setup)
constructor (vt[8]) = 0x088a7090(this=manager, 1):  this+0 = vtable 0x089BABF8(+0x089BAB98);
   jal 0x8812264(global,6);  jal 0x8898748(ctx=[0x09A0B540], slot 4) + (…,5)  = resource reg
```

So the big-monster **manager is a task in the TaskManager linked list**
(`head=[0x08B0C6A4]`, `next=[task+0x18]`); the engine calls each task's **vt[8] to
provision** (when `[task+0x11]==1`) and **vt[0xC] to drive** it per frame. The manager
`0x08B0C7C0` is the singleton built for the primary monster.

**→ The ADD seam: splice a 2nd manager task into the list.** The engine's list-walk then
provisions (vt[8]) + drives (vt[0xC]) it natively. This is `M3b`.

- **N-SAME-family** needs **no species step**: clone the primary's already-provisioned
  manager (same species), splice the copy into the list. Reachable now.
- **N-DIFFERENT-family** needs the same splice **+ step 2** (how a manager learns its
  species so vt[8]/`0x8898748` provisions the right resources). **OPEN** — a multi-vtable
  /multi-setup-fn tangle; needs fresh systematic tracing. *(Dead ends recorded so we
  don't repeat them: `manager+0x0C` is a FLOAT field reset to -NaN by `0x088b9820`, NOT
  the overlay fn — the driver dispatches via vtable[0xC], generic. The early "+0x0C =
  quest-update fn" note is retracted.)*

TaskBase ABI + TaskManager singletons: memory `taskbase-abi-eu`.

## 5. Roadmap (status)

| # | task | status |
|---|------|--------|
| M0 | architecture mapped (this doc) | ✅ |
| M1 | overlay relocator (`tools/ovl_reloc.py`) | ✅ built + validated, byte-correct on HW |
| M2 | runtime loader (`bigmon_overlay.cpp`): native-slot / extra-RAM / volatile placement, relocate, zero bss | ✅ built; native-monster rebind proven |
| RAM | persistent safe storage on stock PPSSPP (`plugin.ini memory=64` → extra RAM) | ✅ **solved** (§6) |
| M3a | redirect a native monster to a relocated overlay (`+0x4C8` rebind) | ✅ proven |
| M3b-same | **N-same-family**: clone manager + splice into TaskManager list | scoped, buildable now |
| M3b-diff | **N-different-family**: splice + **step 2 (species association)** | step 2 OPEN (needs trace) |
| M4 | quest groups → N (`target[2..N]`, `Quest+0x67C=N`) | partly built; `quest_add_monster` is Giadrome-tuned, crashes on big-mon hosts (record layout not generalized) |
| M5 | RAM budget (~1 MB/extra family) — realistic ceiling **2–3** | small |

**Scope reality:** any of the 17 overlay monsters as primary; **N same-family** via the
manager splice; **2–3 different families** if step 2 is cracked (PSP RAM ceiling); plus
unlimited EBOOT small-monsters.

## 6. The RAM wall + where 376KB can live (Section 42, research 2026-06-09)

A 2nd *different-family* overlay needs **~376KB contiguous, held for the duration**.
Survey of every candidate on MHFU (fat 32MB layout):

| region | addr | size | usable? |
|--------|------|------|---------|
| **user partition (2)** | `0x08800000–0x0A000000` | 24MB | MHFU uses ~all → **~141KB free** (too small) |
| **volatile partition (5)** | `0x08400000–0x08800000` | 4MB | free momentarily, but it's the **UMD-decompression scratch** — the game **reclaims/zeros it** (verified: placed copy#2 went to zeros) and holding it disturbs streaming. **Not safe to hold.** |
| **slim extra (8)** | `0x8A000000` | 28MB | **only on PSP-2000+**, kernel-mode, unlock via `SetDdrMemoryProtection`. The fat game never touches it → **safe**. **PPSSPP (fat model) does NOT map it** (probe: `0x0A000000+` unmapped). |
| **slim extra (10)** | `0x8BC00000` | 4MB | same as (8) |
| scratchpad | `0x00010000` | 16KB | far too small |

Hard facts: PSP kernel reserves 4MB + 4MB volatile, leaving **24MB user** on the fat
PSP; **PSP-2000/3000 double RAM to 64MB**, the extra mapping into kernel partitions
**8/10** that retail fat games don't use. **PPSSPP emulates the fat 32MB** and does not
map the slim extra (no user-facing 64MB toggle as of this research).

**Conclusion — safe storage for a sustained 2nd overlay requires one of:**
1. **Real PSP-2000/3000 + CFW + a kernel-mode PRX** → allocate from partition 8
   (`SetDdrMemoryProtection` to unlock, then `sceKernelAllocPartitionMemory(8,…)`).
   28MB free and safe → fits many overlays + models.
2. **Patch the PPSSPP fork** (`tools/ppsspp-fork/`) to map the slim extra RAM
   (`0x0A000000–0x0C000000`) and honor `sceKernelAllocPartitionMemory` for it — then
   the existing loader's partition path works unchanged. Emulator-only, but the fork
   is already ours to build.
3. **Stay on stock PPSSPP** → no safe 376KB; different-family ADD is RAM-blocked.
   Use REPLACE / same-family multi (§8), which need **zero** extra overlay RAM.

(M1/M2 relocation is proven byte-correct; the blocker is purely *where to keep it*.)

## 7. N monsters of the SAME family — SOLVED (verified in-game 2026-06-10)

Same-family monsters **share the single resident overlay, script struct, and model**
— so each *additional* same-family monster costs only its **entity struct (light)**,
**no overlay, no relocation, no model duplication, no extra heavy RAM.** This sidesteps
the §6 wall entirely.

**The mechanism (proven): one big monster = one target GROUP = one manager = one entity.**
To add a 2nd same-family monster, add a 2nd **group** (`Quest+0x67C` 1→2, fill `target[1]`),
NOT a 2nd definition inside group 0. (A 2nd def in group 0 — `target[0]+0x1E` count 2 —
builds fine in `buildTargets` but only `def[0]` instantiates, because the big-mon manager
`0x08B0C7C0` is a fixed **singleton** that drives exactly one monster. Two groups ⇒ the
engine creates two managers ⇒ two entities.)

**Shipped path — `framework/prx/mods/bigmon_dup/mod.cpp`:** in `QUEST_TARGETS_BUILDING`
it calls **`mhfu_quest_add_monster(q, id, x, z)`** (in `framework/prx/src/core/quest.cpp`),
which appends a list-A node cloning the source record (retag is identity for same family),
and whose **buildTargets POSTFIX** moves the 2nd monster into `target[1]` (group 1, count 1),
sets group 0 back to count 1, and raises `Quest+0x67C` to 2 — the exact native dual-big
layout. The "forge" (manual mount/register of a non-resident monster's resources) stays
**disarmed** and is irrelevant here: same family ⇒ resources already resident.

**Verified live** on the native Tigrex quest (*Absolute Power*): two working Tigrex —
registry slot1 `0x090c1b00` @(11209,9493) + slot2 `0x090c9500` @(14172,9649);
`group0 count=1, group1 count=1, +0x67C=2`. The 2nd is **alive, aggros, attacks** = a full
entity running the generic per-entity AI independently. No crash, shared overlay/model.

For **N>2 same family**: add more groups (`+0x67C=N`, `target[2..]`) — the spawn-tile array
per section is the practical ceiling, not RAM. **Entity clone** (memory
`tigrex-clone-recipe`/`monster-spawn-mission`) remains an alternative route but is no longer
needed for the quest-native path.

## 8. Tools + artifacts

- `tools/ovl_reloc.py` — MWo3 parser + relocator (`--selftest`, `--newbase`, `-o`).
  `Overlay.refs()` re-extracts the reference set. **Delta must be 64KB-aligned.**
- `tools/ovl_reloc_validate.py` — spimdisasm cross-check scaffold (superseded by the
  built-in dataflow self-test, kept for reference).
- `framework/prx/src/core/ovl_reloc.cpp` + `include/mhfu/ovl_reloc.h` — C port of the
  relocator (byte-identical to Python; host self-test: `g++ -DOVL_RELOC_TEST ...`).
- `framework/prx/src/core/bigmon_overlay.cpp` + `include/mhfu/bigmon_overlay.h` — the M2
  loader (`mhfu_bigmon_load_relocated`); lua binding `mhfu.load_relocated_overlay(path)`.
- `framework/prx/src/core/quest.cpp` — `mhfu_quest_add_monster` (list-A node + target[1] +
  `Quest+0x67C=2`); the **forge is disarmed** (`g_script_n=0`), script now comes from the
  relocation/manager path. Only Tigrex's record layout is decoded (crashes on other hosts).
- **Stock-PPSSPP RAM grant:** add `memory = 64` under `[options]` in
  `~/.config/ppsspp/PSP/PLUGINS/mhfu_framework/plugin.ini` (built-in PPSSPP plugin feature,
  cap 93 MB; grows raw RAM to `0x0C000000`). No-op on real PSP (use partition 8 there).
- em overlays on disk: `workspace/extracted/data_files/file_06094`–`06110` (em01…em83 in
  the `em [1,2,7,14,15,17,20,21,33,40,54,55,58,59,75,82,83]` order); **em75=`file_06108`**.
- Mods: `mods/lua_host/scripts/` + the memstick `mods/` (`bigmon_diff_family.lua` test, etc.).
  Reference traces: `BOOT.BIN` ELF PH0 vaddr `0x08804000` off `0x25b4` for clean EBOOT disasm.

## 9. Key addresses (quick ref)

| what | addr |
|------|------|
| em overlay slot base / load_addr (em75) | `0x09d15100` / `0x09d1a180` |
| em75 script struct (native) | `0x09D56448` |
| TaskManager** / instance | `0x08A8B1E8` / `0x08B0C6A4` |
| big-mon manager singleton (vtable) | `0x08B0C7C0` (`0x089BABF8`) |
| TaskManager update (list walk) | `0x08895538` (head `[mgr]`, next `[+0x18]`, state `+0x11`) |
| per-frame driver → overlay-init | `0x088971A0` → `0x09a5f768` |
| manager constructor (vt[8], provision) | `0x088a7090` |
| resource-reg fn (in constructor) | `0x8898748(ctx=[0x09A0B540], slot)` |
| added-monster crash (NULL res-ctx) | `0x08859b54` `[NULL+0x19014]` |
| stock-PPSSPP extra RAM | `[0x0A000000, 0x0C000000)` |

## 10. §48 — buildTargets is uncapped; the cap is the single manager (2026-06-10)

Verified live on the native **Tigrex** quest (*Absolute Power / Hunt the Tigrex*).

**buildTargets `0x0886D4D8`** = dispatcher → builds two groups:
`z_un_0886db0c(Quest+0x71C)` (metadata) + `z_un_0886dc68(Quest+0x718)` (big-mon group).
`z_un_0886dc68` is an **uncapped loop** over monster entries (stride `0x3C`, `emId=lh[+0]`,
`-1`=0xFFFF sentinel) → per-target add `z_un_0886d5e4` for every entry; add-fn = `[a2]++` +
`[a1+0x2C]=idx` + species-table read `0x09BB89B0 + emId*0x1D0`, **no limit**. The fn after
(`0x0886d644`) = big-mon emId classifier switch (`0x53 52 4D 51 4B 41 40 3B 3C 36 59…`).

**Live quest structures** (Quest=`0x09A05DC0`):
- list-A: `list_a = recb + [recb+0x14]`; recb=`Quest+0x50`=`0x08A5C440`; node stride `0x10`
  {hdr u32@+0x08 (0=end), rec-off u32@+0x0C}; record {emId u16@+0, coords f32 @+0x20 X /+0x28 Z}.
- target table: count `Quest+0x764`; group0 `Quest+0x768` stride `0x2C` {def[0]@+0x00,
  def[1]@+0x04, emId[] byte@+0x14, count u16@+0x1E}; group1 `Quest+0x794`; group-count `Quest+0x67C`.
- **Post-INIT live edits don't add a spawn** — count is locked at INIT. Edit must be pre-INIT
  (the list buildTargets reads).

**`bigmon_dup` mod** (`framework/prx/mods/bigmon_dup/`, `mhfu_quest_clone_monster` in
`src/core/quest.cpp`): in QUEST_TARGETS_BUILDING (prefix), append a 2nd same-species list-A node
(clone source record → `recb+0x1C00`, offset coords, mon-hdr @ `+0x1C40`, append+re-terminate);
**no** `g_add_quest` (postfix split stays off → both stay in group 0). **Result:** group0
count=2, def[0]=`0x08a5d9e4` def[1]=`0x08a5e040`, both Tigrex, rec0@(11209,9493)
clone@(14209,9493) — **but only 1 entity spawns** (registry: 1× type-0x4B @`0x090c1b00`).

**Cap = the manager, a fixed SINGLETON.** `0x08B0C7C0` vtable `0x089bac6c`, head+sole node of
TaskManager `[0x08B0C6A4]` (`+0x18=0x0b` is data, not a next-ptr). vt[8]=`0x088b9884` provision
(generic), vt[0xC]=`0x088b9910` drive→`z_un_088971a0` (single-object state machine on `+0x11`),
vt[0x10]=`0x088b996c` setup (state @ `+0x34`; registers **fixed** script res `0x47`,`0x4C`; no
group loop). 1 manager ⇒ 1 monster — so a 2nd monster needs a 2nd GROUP (→ 2nd manager), not a
2nd def in group 0.

**RESOLVED.** Switched `bigmon_dup` to `mhfu_quest_add_monster` (2-GROUP split: append list-A
node + postfix moves 2nd into `target[1]`, `+0x67C`→2). Verified live: 2 working Tigrex (slot1
`0x090c1b00` + slot2 `0x090c9500`; group0 count=1, group1 count=1, `+0x67C=2`); 2nd alive +
aggros + attacks. The entity-creation-seam hunt became moot — the 2-group split makes the engine
create the 2nd entity/manager itself. Tools added this session: `src/ppsspp_debug/`
`trace_bigmon_ctor.py`, `trace_bigmon_species.py`, `inject_second_bigmon.py`,
`trace_entity_spawn.py` (freeze-safe log-BP pattern), `paint_loop.py` (wifi-tolerant minimap
paint), `paint_and_count.py`.
