-- mhfu_port.lua — the PORTED-MONSTER RUNTIME. Load a monster ported from
-- MHP3rd (skeleton, mesh, textures, animations) into an MHFU quest, and script
-- its AI so that the move it EXECUTES and the clip you SEE are the same thing.
--
-- ============================================================================
-- 🔴 WHY THIS FILE EXISTS — the two-channel finding (docs/AI_SCRIPTING_ENGINE
-- §33-34, verified live 2026-08-26).
--
-- A big monster runs on TWO INDEPENDENT channels:
--
--   ANIMATION   executor 0x09AC5228(entity, a1)      -> which CLIP plays
--   BEHAVIOUR   act_set  -> entity+0x298 / +0x299    -> which per-action MIPS
--                                                       code runs: the hitbox,
--                                                       the effects, the damage
--
-- Forcing `a1` provably cannot change what an attack DOES. Writing (main, sub)
-- provably runs a complete, damaging move — a Lua-forced pair killed the hunter.
--
-- A port therefore has a specific, nameable defect: the host's behaviour handler
-- asks for host animation id N, and the port's clip that landed in slot N is
-- whatever the packer put there — by POSITION, not by MEANING. The Brute's
-- rock-throw keyframes sat in the Tigrex roar's slot, so the engine roared
-- correctly while the screen showed a rock throw.
--
-- The fix is a DECLARED mapping, and that is what this library is:
--
--   moves = { charge = { main = 3, sub = 6, clip = "charge" } }
--
-- `port:play("charge")` writes the behaviour pair AND latches the clip, so the
-- next executor dispatch is overridden to the port's own charge animation. The
-- host's opinion about which clip belongs to that move is discarded.
--
-- This generalises to a source monster with NO host analogue (a Zinogre): you
-- are not aligning the port to the host, you are picking any host behaviour with
-- the physics you want and putting your own clip on top of it.
-- ============================================================================
--
-- ---------------------------------------------------------------- USING IT
-- A mod file is loaded from ms0:/PSP/PLUGINS/mhfu_framework/mods/ alongside this
-- one, in DIRECTORY ORDER — which is not alphabetical and not guaranteed. So a
-- mod never calls into this library at load time. It registers a setup function
-- and the library calls it back:
--
--   mhfu.port = mhfu.port or { _queue = {} }
--   mhfu.port.mod = mhfu.port.mod or function(n, f) mhfu.port._queue[n] = f end
--
--   mhfu.port.mod("my_mod", function(P)
--     local m = P.define{ ... }
--     m:brain(function(s) ... end)
--   end)
--
-- Those two bootstrap lines are the contract; copy them verbatim. Whichever file
-- loads first creates the namespace, and the library drains the queue when it
-- is ready. Re-registering under the same name REPLACES — so hot-reloading a mod
-- file does not stack duplicate handlers.
--
-- ⚠️ A mod must NOT define `mhfu_tick`: there is one global tick and this
-- library owns it. Use `m:brain(fn)` instead.
--
-- ⚠️ THE TICK IS 2 Hz. lua_host's worker runs at 10 Hz and calls mhfu_tick every
-- 5th iteration (mod.cpp `worker`). Nothing faster is reachable from Lua — the
-- executor hook fires only when a NEW action is dispatched (~0.5/s measured), so
-- it is not a substitute clock. Every reaction threshold in a brain has to
-- absorb 500 ms of monster travel; a charge covers ~650 units in that time.

-- ------------------------------------------------------------- bootstrap
mhfu.port = mhfu.port or { _queue = {} }
mhfu.port.mod = mhfu.port.mod or function(n, f) mhfu.port._queue[n] = f end

local P = mhfu.port
P._queue = P._queue or {}
P.ports  = {}              -- name -> port handle (rebuilt on every mod reload)
P._mods  = P._mods or {}   -- name -> setup fn, kept so a LIBRARY reload can
                           -- rebuild P.ports without touching the mod file
P._once  = P._once or {}   -- side effects that must happen exactly once per boot

-- ------------------------------------------------------------- offsets
-- All from docs/agent_memory_map.md. Kept local and named so a mod never has to
-- know a raw offset to write a brain.
local OFF_POS       = 0x200   -- f32[3] world position
local OFF_YAW       = 0x1F4   -- u16 packed yaw, 0..0xFFFF = 0..2pi. THE rotation
                              -- control cell: transform_builder rebuilds the
                              -- matrix from it every frame, so writing the matrix
                              -- directly gets clobbered and writing this does not.
local OFF_MAIN      = 0x298   -- act_set main_state
local OFF_SUB       = 0x299   -- act_set sub_state
local OFF_PREV_MAIN = 0x460   -- act_set stores the outgoing pair here; handlers
local OFF_PREV_SUB  = 0x461   -- read it on transitions
local OFF_PHASE     = 0x1D5   -- per-action phase cursor (+0x1D5..+0x1D7)
local OFF_SECTION   = 0x29A   -- u16, compares against get_area_index()
local OFF_HP        = 0x41E
local OFF_FREEZE    = 0x4B8
-- The two cells that say WHO the monster has committed to, as opposed to
-- whether it has noticed anyone. See `targets_player` in the state table.
local OFF_TARGET    = 0x2F4   -- u32, resolved combat target pointer
local OFF_ACQUIRED  = 0x2A4   -- u8, aggro-eval target-acquired flag
local FREEZE_BITS   = 0x100 | 0x10000

-- 🔴 The player's WORLD position is the combat entity's transform row 3.
-- `mhfu.player_pos()` reads 0x09998D50, which is the CAMERA EYE — it reported
-- d=26000 for a monster that was 365 units away. Framework bug; fixing the
-- header needs a PRX rebuild, which is a `make -C framework/prx` away (the
-- toolchain is the `pspdev/pspdev:latest` Docker image and it is on this box) —
-- nobody has done it. Until then the correct address is used directly here.
local PLAYER_ENT  = 0x090B3440
local PLAYER_XYZ  = 0x40

-- ------------------------------------------------------------- floats
-- 🔴 THE C BINDINGS ARE CORRECT. THE HAND-ROLLED DECODER IN THIS REPO IS NOT.
--
-- Every Lua script here decodes floats itself with the idiom
--
--     local sign = 1.0; if u >= 0x80000000 then sign = -1.0; u = u - 0x80000000 end
--
-- and on the PSP build that test is ALWAYS TRUE. lua_Integer is 32 bits there
-- (`[lua_host] VM ready: lua_Number=4B`), so the literal `0x80000000` wraps to
-- -2147483648 and every non-negative u compares greater. The subtraction then
-- wraps the top bit back on, so the exponent and mantissa survive intact and the
-- result is a CLEAN SIGN FLIP of the true value.
--
-- It hid for months because the only thing anyone computed from these floats was
-- a DISTANCE, and negating both endpoints leaves the distance unchanged. It
-- surfaced the moment something WROTE a position back: the pin stored the
-- monster where he stood, wrote the mirror image, and teleported him ~30 000
-- units away — the mod's own reads stayed self-consistent (flip in, flip out)
-- while the screen showed an empty snowfield and the debugger measured 32 000.
--
-- ⚠️ This inverts what the previous version of this comment claimed. A one-time
-- `f32 check hand=-8957 binding=8957` was read as "the binding drops the sign".
-- It was the other way round, and the debugger settled it: the same entity read
-- (9849, 1000, 8056) over the wire and (-12928, -1000, -9459) from Lua.
--
-- So: use `mhfu.read_f32` / `mhfu.write_f32`. The hand decoder below is kept
-- only for the one-time cross-check at spawn, and it is the FIXED form —
-- `(u & 0x80000000) ~= 0` works whatever width lua_Integer has.
local function u32_to_float(u)
  local sign = ((u & 0x80000000) ~= 0) and -1.0 or 1.0
  local exp, mant = (u >> 23) & 0xFF, u & 0x7FFFFF
  if exp == 0   then return sign * mant * (2.0 ^ -149) end
  if exp == 255 then return sign * math.huge end
  return sign * (1.0 + mant * (2.0 ^ -23)) * (2.0 ^ (exp - 127))
end
local function rf(a) return mhfu.read_f32(a) end
local function wf(a, v) mhfu.write_f32(a, v) end

-- Two-argument math.atan is 5.3+; math.atan2 is the 5.1/5.2 spelling. The host
-- Lua uses bitwise operators elsewhere so it is 5.3+, but this costs nothing.
local atan2 = math.atan2 or math.atan

-- ------------------------------------------------------------- logging
-- 🔴 TWO traps, both silent:
--   1. mhfu.log takes a SINGLE string — it does not printf, extra args vanish.
--   2. mhfu.log from SCRIPT LOAD or from an event CALLBACK never reaches
--      framework.log; only the mhfu_tick worker context does.
-- So everything queues and the tick drains it.
local PEND, PEND_N = {}, 0
local function log(fmt, ...)
  PEND_N = PEND_N + 1
  PEND[PEND_N] = (select("#", ...) == 0) and fmt or string.format(fmt, ...)
end
P.log = log

local function drain()
  if PEND_N == 0 then return end
  for i = 1, PEND_N do mhfu.log(PEND[i]); PEND[i] = nil end
  PEND_N = 0
end

-- ------------------------------------------------------------- act_set
-- act_set (0x09AC8690) reimplemented with plain memory writes, because the
-- framework has no native-call binding for it. ⚠️ That is a gap, not a wall: the
-- PRX builds from `pspdev/pspdev:latest` via `make -C framework/prx` and the
-- image is on this box, so a real binding — and a tick faster than 2 Hz — is
-- work rather than a blocker. The engine's version also clears the per-slot
-- cursors behind two
-- condition checks; this does the unconditional part, which is the minimum a
-- handler needs to run from its first phase. Measured: a pair written this way
-- ran a complete attack and killed the hunter.
--
-- ⚠️ PULSED, never per-tick. Rewriting the pair every tick restarts the move
-- before it reaches its hitbox frames (the same failure a held a1-force showed),
-- and repeated forcing makes the engine OR in the exhaustion bits and halt the
-- AI tick entirely — so the bits are cleared on each pulse.
local function act_set(ent, main, sub)
  mhfu.write_u8(ent + OFF_PREV_MAIN, mhfu.read_u8(ent + OFF_MAIN))
  mhfu.write_u8(ent + OFF_PREV_SUB,  mhfu.read_u8(ent + OFF_SUB))
  mhfu.write_u8(ent + OFF_MAIN, main)
  mhfu.write_u8(ent + OFF_SUB,  sub)
  mhfu.write_u8(ent + OFF_PHASE,     0)
  mhfu.write_u8(ent + OFF_PHASE + 1, 0)
  mhfu.write_u8(ent + OFF_PHASE + 2, 0)
  local g = mhfu.read_u32(ent + OFF_FREEZE)
  if (g & FREEZE_BITS) ~= 0 then mhfu.write_u32(ent + OFF_FREEZE, g & ~FREEZE_BITS) end
end
P.act_set = act_set

-- ------------------------------------------------------------- Port handle
local Port = {}
Port.__index = Port

--- Declare a ported monster.
--
--   name    identifier, also the hot-reload key
--   species host species id the port rides on (mhfu.MON_TIGREX)
--   pac     ported PAC filename under the framework's inject/ directory
--   orig    the original PAC it replaces, same directory
--   fid     cosmetic file id for the injector (matching is by content)
--   replace list of quest monster ids to swap for `species`
--   clips   name -> executor a1. THE PORT'S OWN animation vocabulary.
--   moves   name -> { main, sub, clip } — the ALIGNMENT: which host behaviour
--           runs, and which of the port's clips is shown while it does.
--
-- Returns a port handle. Safe to call again (hot reload): the injector is only
-- armed once per boot.
function P.define(spec)
  -- 🔴 A redefine must NOT drop the monster. `define` runs again on every hot
  -- reload of the mod file, and the spawn event that bound the entity fired long
  -- ago — so a fresh handle with ent = 0 silently stops the brain, in a quest
  -- where nothing will ever spawn again. Carry the binding over; the rest of the
  -- state is meant to reset.
  local prev = P.ports[spec.name]
  local self = setmetatable({
    name    = spec.name,
    species = spec.species,
    clips   = spec.clips or {},
    moves   = spec.moves or {},
    replace = spec.replace or {},
    ent     = prev and prev.ent or 0,
    clip    = nil,      -- currently latched executor a1, nil = hands off
    move    = nil,      -- currently scripted move name
    pinned  = nil,      -- {x, y, z} while the coordinate lock is on
    -- How long the last scripted move actually SURVIVED, in ticks. This is the
    -- feedback a brain needs to pick behaviour pairs by measurement instead of
    -- by hope: a forced pair whose handler declines the situation ends on its
    -- first tick, and the only way to find that out is to try it and count.
    last_move = nil, last_move_ticks = 0,
    slip    = 0,        -- units the pin had to correct on the last tick
    _brain  = nil,
  }, Port)

  if spec.pac and not P._once["inject:" .. spec.name] then
    P._once["inject:" .. spec.name] = true
    local dir  = spec.inject_dir or "ms0:/PSP/PLUGINS/mhfu_framework/inject"
    local ok = mhfu.inject_relocate(spec.fid, dir .. "/" .. spec.pac,
                                    dir .. "/" .. spec.orig)
    log("[port:%s] inject_relocate %s fid=%d '%s'", spec.name,
        ok and "OK" or "FAILED", spec.fid, spec.pac)
  end

  P.ports[spec.name] = self
  return self
end

--- Register the per-tick brain. `fn(state)` where state carries the monster,
--- the player, the distance between them and the live behaviour pair.
function Port:brain(fn) self._brain = fn; return self end

--- Run a declared move: write the behaviour pair AND latch the port's clip.
--- This is the whole point of the library — one call moves both channels.
---
--- ⚠️ Refuses to re-issue the SAME move within `min_gap` ticks. A brain that
--- re-pulses the moment a move ends is a per-tick force in disguise, and
--- re-entering the executor every tick restarts the move before it reaches its
--- hitbox frames — the exact failure a held a1-force already demonstrated. It
--- also makes the engine OR in the exhaustion bits and halt the AI outright.
function Port:play(move_name, min_gap)
  local mv = self.moves[move_name]
  if not mv then log("[port:%s] no such move '%s'", self.name, tostring(move_name)); return false end
  if self.ent == 0 then return false end
  min_gap = min_gap or 2
  if self._played == move_name and (self._tick - (self._played_at or -99)) < min_gap then
    return false
  end
  act_set(self.ent, mv.main, mv.sub)
  self.move, self._played, self._played_at = move_name, move_name, self._tick
  self.clip = mv.clip and self.clips[mv.clip] or mv.anim
  -- 🔴 HOW MANY DISPATCHES THE CLIP OVERRIDE IS GOOD FOR, and one is the right
  -- answer. A forced pair does not correspond to a single executor dispatch:
  -- filmed live, one `(2,1)` lasting seven ticks asked the executor for a1
  -- 15, 11, 19 and 18 in turn — the handler runs a SEQUENCE of sub-actions.
  -- Overriding all of them restarts the port's clip from frame 0 each time,
  -- which is the same held-force failure that made the first build look like
  -- "no animation ever plays to the end", arriving by a different route. So the
  -- latch covers the dispatch that OPENS the move (act_set has just zeroed the
  -- phase cursor, so that is the move's first action) and then abstains, and the
  -- engine's own choice stands for the rest. `latch = <n>` on a move buys more.
  self._clip_uses = mv.latch or 1
  return true
end

--- Latch a clip on the ANIMATION channel alone — no behaviour write.
---
--- `play()` is what a brain should use: it moves both channels together, which
--- is the whole point of the library. This is the instrument underneath it, and
--- it exists for one job — asking "which clip is in slot N of THIS build, and
--- does it play?" without the behaviour handler's opinion in the measurement.
---
--- The override lands on the next executor dispatch, not immediately: the hook
--- only fires when the engine picks a NEW action (~0.5/s), so a latch set while
--- the monster is mid-action waits for the next one. `uses` defaults to 1 for
--- the same reason `play` does — overriding every dispatch of a multi-action
--- move restarts the clip from frame 0 each time, which reads on screen as an
--- animation that never finishes.
---
--- ⚠️ NOT for shipping mods. A clip latched with no behaviour under it paints
--- your animation over whatever the AI is doing, which is exactly the
--- clip/behaviour mismatch `mhfu_port` exists to remove.
function Port:latch(a1, uses)
  if self.ent == 0 then return false end
  self.clip, self._clip_uses = a1, uses or 1
  return true
end

--- Hand both channels back to the engine's own AI.
function Port:release()
  self.move, self.clip, self._played, self._clip_uses = nil, nil, nil, 0
  self:unpin()
end

--- Point the monster at (x, z). Writes +0x1F4 only, so the engine's own VFPU
--- rotator renders the turn — no moonwalk, no matrix fight.
---
--- 🔴 THE ANGLE IS `atan2(dx, dz)`, AND GETTING THAT WRONG IS INVISIBLE UNTIL YOU
--- MEASURE IT. This used to compute `-atan2(z - mz, x - mx)`, which is the same
--- angle measured from the other axis and in the other direction — exactly 90
--- degrees out. The symptom, reported from play: "his crazy forward dash goes
--- first seemingly towards me and then away from me".
---
--- It was blamed on the write being contested (`transform_builder` rebuilds the
--- matrix from this cell every frame, and the species AI writes it too), and
--- that was wrong: the cell is obeyed exactly. One logged charge, monster at
--- (13114, 8642), hunter at (11201, 6101):
---
---     toward the hunter : -143.0 deg
---     Port:face wrote   : +127.0 deg
---     he actually moved : +126.8 deg      <- 0.2 deg from what was written
---
--- The engine's convention is the same one `mhfu_bot.navigation` uses for the
--- player (`yaw = atan2(m20, m22)`, i.e. atan2 of the forward vector's X over
--- its Z), and `+0x1F4` packs 0..0xFFFF over 0..2pi.
function Port:face(x, z)
  if self.ent == 0 then return end
  local mx = rf(self.ent + OFF_POS)
  local mz = rf(self.ent + OFF_POS + 8)
  local hw = math.floor(atan2(x - mx, z - mz) * 32768.0 / math.pi) % 65536
  mhfu.entity_set_yaw(self.ent, hw)
end

--- Lock the monster's XZ where it stands. The Y is left alone: the engine drives
--- it to the local floor every frame and fighting that makes a monster sink.
---
--- 🔴 PIN A PAIR THAT ONLY DRIFTS, AND WATCH THE NUMBER. This rewrites the
--- position at 2 Hz while the engine advances it every frame, so the lock is
--- always undoing something; what matters is how much. Against a pursuit state
--- it is a tug of war the player can see — a play session logged 526-646 units
--- corrected on EVERY tick, and on screen the monster slid forward and snapped
--- back twice a second. Against a pair the engine actually dwells in it is ~45.
---
--- ⚠️ Do NOT expect to retire it. The plan was to drop the pin once the pair
--- underneath was stationary, and the measurement said no pair is: the census
--- called (2,1) "HOLDS + STATIONARY" at 45 units/TICK under a threshold of 60,
--- but the tick is 2 Hz, so that is 90 units a SECOND — a probe that held (2,1)
--- continuously walked the Brute 10 952 -> 31 164 units off the map in 450 s.
--- The census now says STILL under 25/tick and DRIFTS up to 60.
---
--- ⚠️ It is also a per-tick write, and CLAUDE.md rule 8 says never maintain a big
--- monster per-tick. The rule is about UNCONDITIONAL maintenance of +0x29A /
--- +0x638 / size / the freeze gate; this is conditional and scoped to one brain
--- phase. `unpin` logs the running total — divide by the tick count and compare
--- to 45 (fine) and to 526 (the wrong pair underneath).
function Port:pin()
  if self.ent == 0 or self.pinned then return end
  self.pinned = { rf(self.ent + OFF_POS),
                  rf(self.ent + OFF_POS + 4),
                  rf(self.ent + OFF_POS + 8) }
end
function Port:unpin()
  if self.pinned and (self._slip_n or 0) > 0 then
    log("[port:%s] pin released after correcting %d ticks, %d units total",
        self.name, self._slip_n, math.floor(self._slip_sum or 0))
  end
  self.pinned, self._slip_n, self._slip_sum = nil, 0, 0
end

function Port:hold_pin()
  local p = self.pinned
  if not p or self.ent == 0 then return 0 end
  local x = rf(self.ent + OFF_POS)
  local z = rf(self.ent + OFF_POS + 8)
  local dx, dz = x - p[1], z - p[3]
  local slip = math.floor(math.sqrt(dx * dx + dz * dz))
  if slip > 0 then
    wf(self.ent + OFF_POS,     p[1])
    wf(self.ent + OFF_POS + 8, p[3])
  end
  return slip
end

-- ------------------------------------------------------------- hit tables
-- Where he can be hit, and for how much — the port's own, written into the game
-- (issue #19). The editor exports `ports/<name>.toml`'s [[hurtbox]] / [[hitzone]]
-- as a generated `<name>_hit.lua` that calls `P.hit(port_name, tbl)`; this is the
-- side that lands it.
--
-- TWO TABLES, ONE NUMBER EACH, both in game_task.ovl's species row
-- (0x09BB87C0 + species*0x1D0; docs/agent_memory_map.md "THE PART SYSTEM"):
--
--   +0x240  u32 -> the collision-record set THIS species walks. The overlay holds
--                  several (em75: four, one per species id 75/76/81/88) and
--                  references none of them; this pointer is the only edge. Both
--                  engine walkers (0x09C37F30 game_sub, 0x09A84AFC game_task) read
--                  0x28-byte records from here until `bone == 0xFFFF`.
--   +0x2FC  u32 -> the hitzone STATE pointer table; each entry -> a 0x48 block of
--                  7 rows x 10 percentage bytes. Proven live 2026-06-28.
--
-- The volumes go IN PLACE: over the original records, then a sentinel. A SHORTER
-- list is fine (the walker stops at our sentinel); a LONGER one is not — the
-- bytes past the original sentinel are someone else's — so the ORIGINAL count
-- is measured on first contact and is the cap. Nothing is relocated and no
-- pointer is rewritten, which is also why this needs no PRX change.
--
-- ⚠️ Species data is MAP-WIDE and per species id: with the port REPLACING the
-- host it is the only em75 in the quest, so this is his table alone. Riding
-- beside a native Tigrex would re-skin the native's hurtboxes too.
--
-- Applied once the port's entity is live in-area, re-checked every tick against
-- one record and one grid byte, and re-applied (with a log line) if either
-- changed under us — a reload of the overlay would show up here, not as a
-- silent revert.
local SPECIES_TABLE  = 0x09BB87C0
local SPECIES_STRIDE = 0x1D0
local F_SPHERES      = 0x240
local F_STATES       = 0x2FC
local REC            = 0x28
local GRID_BLOCK     = 0x48
local GRID_ROWS, GRID_COLS = 7, 10
local SENTINEL       = 0xFFFF
local MAX_RECORDS    = 512    -- a walk bound; the longest set in the game is 49

P._hit = P._hit or {}         -- port name -> table, from the generated module

--- Register a port's hit tables. `tbl` = { species, id, volumes = {{bone, shape,
--- row, part, flags, radius, ax, ay, az, bx, by, bz}, ...} | nil, grid = {{row*7}
--- x states} | nil }. Keyed by PORT name so the data module and the brain module
--- can load in either order; the tick joins them.
function P.hit(name, tbl)
  P._hit[name] = tbl
  local port = P.ports[name]
  if port then port._hit_id = nil end       -- a re-export applies on the next tick
  log("[port:%s] hit tables registered: %s volume(s), %s grid state(s), id %s",
      name, tbl.volumes and #tbl.volumes or "no", tbl.grid and #tbl.grid or "no",
      tostring(tbl.id))
end

local function species_row(port)
  return SPECIES_TABLE + port.species * SPECIES_STRIDE
end

local function write_record(at, r)
  mhfu.write_u16(at + 0x00, r[1])          -- bone
  mhfu.write_u16(at + 0x02, r[2])          -- shape
  mhfu.write_u16(at + 0x04, r[3])          -- hitzone_row
  mhfu.write_u16(at + 0x06, r[4])          -- part
  mhfu.write_u32(at + 0x08, r[5])          -- flags
  wf(at + 0x0C, r[6])                      -- radius
  wf(at + 0x10, r[7]); wf(at + 0x14, r[8]); wf(at + 0x18, r[9])     -- offset A
  wf(at + 0x1C, r[10]); wf(at + 0x20, r[11]); wf(at + 0x24, r[12])  -- offset B
end

local function write_sentinel(at)
  for i = 0, 3 do mhfu.write_u16(at + i * 2, SENTINEL) end
  for i = 8, REC - 4, 4 do mhfu.write_u32(at + i, 0) end
end

--- Does the live table still carry what we wrote? One record and one byte —
--- cheap enough for every tick, specific enough to catch a reload.
local function hit_intact(port, tbl)
  local row = species_row(port)
  if tbl.volumes then
    local base = mhfu.read_u32(row + F_SPHERES)
    if base == 0 then return false end
    local n = math.min(#tbl.volumes, port._hit_cap or #tbl.volumes)
    if n == 0 then
      if mhfu.read_u16(base) ~= SENTINEL then return false end
    else
      local r = tbl.volumes[1]
      if mhfu.read_u16(base) ~= r[1] then return false end
      if math.abs(rf(base + 0x0C) - r[6]) > 0.01 then return false end
      if mhfu.read_u16(base + n * REC) ~= SENTINEL then return false end
    end
  end
  if tbl.grid and tbl.grid[1] then
    local stt = mhfu.read_u32(row + F_STATES)
    if stt == 0 then return false end
    local blk = mhfu.read_u32(stt)
    if blk == 0 or mhfu.read_u8(blk + 1) ~= tbl.grid[1][1][2] then return false end
  end
  return true
end

local function hit_apply(port, tbl)
  local row = species_row(port)
  local wrote = {}
  if tbl.volumes then
    local base = mhfu.read_u32(row + F_SPHERES)
    if base == 0 or not mhfu.mem_valid(base) then
      return false, string.format("no set pointer at 0x%08X", row + F_SPHERES)
    end
    -- the cap is the ORIGINAL record count, measured before the first overwrite
    -- and kept in P._once: a library hot-reload rebuilds the port handle, and
    -- re-measuring then would count OUR shorter table and shrink the cap for
    -- the rest of the boot.
    local capkey = "hitcap:" .. tostring(port.species)
    if not P._once[capkey] then
      local n = 0
      while n < MAX_RECORDS and mhfu.read_u16(base + n * REC) ~= SENTINEL do
        n = n + 1
      end
      P._once[capkey] = n
      log("[port:%s] host set 0x%08X holds %d record(s) — the in-place cap",
          port.name, base, n)
    end
    port._hit_cap = P._once[capkey]
    local n = #tbl.volumes
    if n > port._hit_cap then
      log("[port:%s] ⚠️ %d volume(s) but only %d fit in place — TRUNCATED",
          port.name, n, port._hit_cap)
      n = port._hit_cap
    end
    for i = 1, n do write_record(base + (i - 1) * REC, tbl.volumes[i]) end
    write_sentinel(base + n * REC)
    wrote[#wrote + 1] = string.format("%d volume(s) @0x%08X", n, base)
  end
  if tbl.grid then
    local stt = mhfu.read_u32(row + F_STATES)
    if stt == 0 or not mhfu.mem_valid(stt) then
      return false, string.format("no state table at 0x%08X", row + F_STATES)
    end
    -- the state count is stored nowhere: the pointer table sits right after
    -- the last block it points at, so it is (table - first_block) / 0x48
    local b0 = mhfu.read_u32(stt)
    local have = (stt - b0) // GRID_BLOCK
    if have < 1 or have > 8 then
      return false, string.format("state table 0x%08X -> 0x%08X: %d states?",
                                  stt, b0, have)
    end
    local n = math.min(#tbl.grid, have)
    if #tbl.grid ~= have then
      log("[port:%s] grid: manifest has %d state(s), the species %d — writing %d",
          port.name, #tbl.grid, have, n)
    end
    for s = 1, n do
      local blk = mhfu.read_u32(stt + (s - 1) * 4)
      for r = 1, GRID_ROWS do
        local rowv = tbl.grid[s][r]
        for c = 1, GRID_COLS do
          mhfu.write_u8(blk + (r - 1) * GRID_COLS + (c - 1), rowv[c])
        end
      end
    end
    wrote[#wrote + 1] = string.format("%d grid state(s) @0x%08X", n, b0)
  end
  return true, table.concat(wrote, ", ")
end

--- Called from the tick for a live, in-area port. Applies on the first
--- opportunity and whenever the live bytes stop matching.
local function hit_tick(port)
  local tbl = P._hit[port.name]
  if not tbl then return end
  if port._hit_id == tbl.id and hit_intact(port, tbl) then return end
  local why = port._hit_id == tbl.id and "live table changed under us"
           or (port._hit_id and "new export" or "first contact")
  local ok, what = hit_apply(port, tbl)
  if ok then
    port._hit_id = tbl.id
    log("[port:%s] HIT TABLES APPLIED (%s): %s  id=%s", port.name, why, what,
        tostring(tbl.id))
  else
    port._hit_id = nil
    if (port._hit_fail or 0) % 20 == 0 then
      log("[port:%s] hit tables NOT applied: %s", port.name, tostring(what))
    end
    port._hit_fail = (port._hit_fail or 0) + 1
  end
end

-- ------------------------------------------------------------- events
-- Registered ONCE, at library load, and they dispatch to whatever ports happen
-- to be defined at the time. That is what makes hot-reloading a mod file safe:
-- the mod's setup runs again, P.ports is rebuilt, and no handler is ever stacked.

if not P._once.events then
P._once.events = true

-- Every hit the port takes, with the amount: the number issue #19 is measured
-- in. Fires from the 5 Hz monster poll (an HP-drop edge), so two hits inside
-- 200 ms arrive as one line with their sum.
mhfu.on_bigmonster_damaged(function(ent, mtype, amount, hp, slot)
  for _, port in pairs(P.ports) do
    if port.ent == ent then
      port._hits = (port._hits or 0) + 1
      log("[port:%s] HIT #%d  -%d  hp=%d", port.name, port._hits, amount, hp)
    end
  end
end)

mhfu.on_quest_targets_building(function(quest)
  if quest == 0 then return end
  for name, port in pairs(P.ports) do
    for _, victim in ipairs(port.replace) do
      if mhfu.quest_has(quest, victim) then
        local ok = mhfu.quest_replace_monster(quest, victim, port.species)
        log("[port:%s] swap %d -> %d %s", name, victim, port.species,
            ok and "applied" or "FAILED")
      end
    end
  end
end)

mhfu.on_bigmonster_spawn(function(ent, mtype, slot, hp)
  for name, port in pairs(P.ports) do
    if mtype == port.species and port.ent == 0 then
      port.ent = ent
      log("[port:%s] SPAWN ent=0x%08X slot=%d hp=%d sec=%d pos=(%d,%d,%d)",
          name, ent, slot, hp, mhfu.read_u16(ent + OFF_SECTION),
          math.floor(rf(ent + OFF_POS)), math.floor(rf(ent + OFF_POS + 4)),
          math.floor(rf(ent + OFF_POS + 8)))
      -- one-time: the two decoders must agree. If they ever diverge again,
      -- believe the debugger, not either of them. (see 'floats')
      log("[port:%s] f32 check  binding=%d  hand=%d", name,
          math.floor(mhfu.read_f32(ent + OFF_POS)),
          math.floor(u32_to_float(mhfu.read_u32(ent + OFF_POS))))
    end
  end
end)

mhfu.on_bigmonster_death(function(ent)
  for _, port in pairs(P.ports) do
    if port.ent == ent then port.ent = 0; port:release() end
  end
end)

-- THE ALIGNMENT SEAM. The host behaviour handler has just asked the executor for
-- its own idea of the right clip; if a scripted move is active we substitute the
-- port's. Returning nil abstains and the engine's choice stands.
mhfu.on_bigmonster_action(function(ctx)
  for _, port in pairs(P.ports) do
    if ctx.entity == port.ent and port.clip and (port._clip_uses or 0) > 0 then
      port._clip_uses = port._clip_uses - 1
      if port.clip ~= ctx.action_id then
        log("[port:%s] clip %d -> %d  (move=%s)", port.name, ctx.action_id,
            port.clip, tostring(port.move))
      end
      return port.clip
    end
  end
  return nil
end, 10)

end  -- P._once.events

-- ------------------------------------------------------------- the tick
local g_tick = 0

local function port_state(port)
  -- travelled-since-last-tick is the only speed signal a 2 Hz brain gets, and it
  -- is what tells a charge from a walk: a charge covers ~650 units per tick, the
  -- pursuit walk ~180. Measured off framework.log across 60k logged ticks.
  local ent = port.ent
  local mx = rf(ent + OFF_POS)
  local my = rf(ent + OFF_POS + 4)
  local mz = rf(ent + OFF_POS + 8)
  local px = rf(PLAYER_ENT + PLAYER_XYZ)
  local py = rf(PLAYER_ENT + PLAYER_XYZ + 4)
  local pz = rf(PLAYER_ENT + PLAYER_XYZ + 8)
  local sec, area = mhfu.read_u16(ent + OFF_SECTION), mhfu.get_area_index()
  -- 🔴 EVERY SECTION HAS ITS OWN WORLD FRAME, so a position from before a
  -- section change cannot be subtracted from one after it. A live take logged
  -- `travelled=15324` on the tick the hunter crossed a boundary, which fed a
  -- predictive abort a speed of 15000 units/tick and fired it on nonsense.
  -- Distances are only meaningful within one frame; the first tick after a
  -- change has no valid previous sample.
  local dist = math.sqrt((mx - px) ^ 2 + (mz - pz) ^ 2)
  local last = port._last
  local travelled, closing = 0, 0
  if last and last[3] == sec then
    travelled = math.sqrt((mx - last[1]) ^ 2 + (mz - last[2]) ^ 2)
    -- 🔴 CLOSING, NOT TRAVELLED, is what a "will he reach the hunter" projection
    -- has to be built on. They are the same number only when the hunter stands
    -- still: a hunter walking INTO a charge shrinks the gap by his speed as well
    -- as the monster's, and a dry run of exactly that had the Brute abort a tick
    -- late and finish at d=189 with SAFE set to 320. `travelled` still earns its
    -- place — it is the signal that says which behaviour pair is running — but
    -- the abort line takes whichever of the two is larger.
    closing = last[4] - dist
  end
  port._last = { mx, mz, sec, dist }
  local target = mhfu.read_u32(ent + OFF_TARGET)
  return {
    travelled = travelled, closing = closing,
    move = port.move, pinned = (port.pinned ~= nil),
    slip = port.slip,
    -- what the LAST scripted move achieved, so a brain can drop a pair the
    -- engine refuses instead of re-issuing it forever
    last_move = port.last_move, last_move_ticks = port.last_move_ticks,
    since_play = g_tick - (port._played_at or -999),
    port = port, ent = ent, tick = g_tick,
    x = mx, y = my, z = mz, px = px, py = py, pz = pz,
    dist = dist,
    section = sec, area = area, same_section = (sec == area),
    -- true on the first tick of a new frame: dist is readable, travelled is not
    reframed = (last == nil or last[3] ~= sec),
    -- ⚠️ NOT full combat mode. +0x5DC comes up when the monster has DETECTED you
    -- and is pursuing — the '!' over its head. Played by hand, a swapped Brute
    -- showed the '!' and pursued but the yellow eye never appeared next to the
    -- hunter's name.
    --
    -- ⛔ The reason is NOT "the swap leaves the combat target unwired" — that
    -- was retracted (a misread of the target-table base). The target is wired,
    -- to the CAT: `+0x542` is a priority index and the Felyne outranks the
    -- hunter, so `+0x2F4` sits on the cat ~6 s and flicks to the player ~0.1 s,
    -- measured as PLAYER 0-4 % of samples. The yellow eye is that 0.1 s window,
    -- too short to see. A native Tigrex in a quest with no Felyne reads PLAYER
    -- 100 % and shows a solid eye (savestate `tigrex_s6`).
    --
    -- Gate on this and you are gating on "has noticed you".
    engaged = mhfu.entity_engaged(ent),
    -- 🔴 WHO he has committed to, which is NOT what `engaged` says.
    -- `+0x2F4` is the resolved combat target pointer, selected by the priority
    -- index `+0x542` (0 = player, 1 = cat). It is a DIAGNOSTIC, not a gate: it
    -- oscillates several times a second up close (CAT 44 % / PLAYER 42 % in one
    -- take) and the cat wins outright at range (CAT 100 %), so a brain that
    -- gates on `targets_player` stutters.
    --
    -- ⚠️ A native Tigrex reads PLAYER 100 %, but do NOT read that as
    -- native-vs-swap: the savestate it was measured on (`tigrex_s6`) has no
    -- Felyne at all (`0x090BDC40` reads vtable 0, HP 0, position NaN). It is
    -- cat-present vs cat-absent.
    --
    -- `+0x2A4` is the aggro-eval's target-acquired flag. ⛔ It is NOT the
    -- combat-mode read — it is 0 on a native Tigrex that is engaged, targeting
    -- the player and mid-attack with the yellow eye plainly visible.
    target = target, targets_player = (target == PLAYER_ENT),
    acquired = mhfu.read_u8(ent + OFF_ACQUIRED),
    main = mhfu.read_u8(ent + OFF_MAIN), sub = mhfu.read_u8(ent + OFF_SUB),
    hp = mhfu.read_u16(ent + OFF_HP),
    player_hp = mhfu.get_player_hp(),
  }
end

function mhfu_tick()
  drain()
  mhfu.paint_map()
  g_tick = g_tick + 1

  -- Mods registered before this file loaded are still sitting in the queue.
  if next(P._queue) then
    for name, fn in pairs(P._queue) do
      P._queue[name] = nil
      local ok, err = pcall(fn, P)
      log("[port] mod '%s' setup %s", name, ok and "ok" or ("FAILED: " .. tostring(err)))
    end
    drain()
  end

  for _, port in pairs(P.ports) do
    port._tick = g_tick
    -- Belt and braces for the same problem: if we have no entity but one of the
    -- port's species is live, adopt it. Covers a spawn event missed for any
    -- reason, not just a reload.
    if port.ent == 0 then
      local live = mhfu.entities_of_type(port.species)
      if live and live[1] and live[1] ~= 0 then
        port.ent = live[1]
        log("[port:%s] adopted live entity 0x%08X (no spawn event)", port.name, port.ent)
      end
    end
    if port.ent ~= 0 and mhfu.entity_alive(port.ent) then
      -- 🔴 DROP THE CLIP WHEN THE MOVE IS OVER. The latch overrides EVERY
      -- executor dispatch while it is set, so leaving it on past the move paints
      -- the port's charge animation over whatever the engine does next — which
      -- is precisely the clip/behaviour mismatch this library exists to remove.
      -- The behaviour pair is the ground truth: when the engine has moved off
      -- the pair we wrote, our move is finished.
      if port.move then
        local mv = port.moves[port.move]
        if mv and (mhfu.read_u8(port.ent + OFF_MAIN) ~= mv.main
                or mhfu.read_u8(port.ent + OFF_SUB) ~= mv.sub) then
          local held = g_tick - (port._played_at or g_tick)
          log("[port:%s] move '%s' ended after %d ticks -> (%d,%d)", port.name,
              port.move, held,
              mhfu.read_u8(port.ent + OFF_MAIN), mhfu.read_u8(port.ent + OFF_SUB))
          port.last_move, port.last_move_ticks = port.move, held
          port.move, port.clip, port._clip_uses = nil, nil, 0
        end
      end
      if port.pinned then
        port.slip = port:hold_pin()
        -- ⚠️ ONE LINE PER PIN, NOT ONE PER TICK. The pin runs at 2 Hz for as long
        -- as a phase lasts; logging each correction buried a 2200-line take under
        -- 152 identical lines and made the interesting events unfindable. The
        -- running total is the number that matters anyway: at 0 the pin is doing
        -- nothing and can go.
        port._slip_n = (port._slip_n or 0) + (port.slip > 4 and 1 or 0)
        port._slip_sum = (port._slip_sum or 0) + port.slip
        if port.slip > 40 and (port._slip_n == 1 or port._slip_n % 20 == 0) then
          log("[port:%s] pin has corrected %d ticks, %d units total (last %d) t=%d",
              port.name, port._slip_n, math.floor(port._slip_sum), port.slip, g_tick)
        end
      else
        port.slip = 0
      end
      -- the hit tables land once he is live in-area, and are re-checked here
      if mhfu.get_screen_state() == 17 then
        local ok, err = pcall(hit_tick, port)
        if not ok then log("[port:%s] hit error: %s", port.name, tostring(err)) end
      end
      if port._brain then
        local ok, err = pcall(port._brain, port_state(port))
        if not ok then log("[port:%s] brain error: %s", port.name, tostring(err)) end
      end
    elseif port.ent ~= 0 then
      port.ent = 0; port:release()
    end
  end
  drain()
end

-- ------------------------------------------------------------- ready
-- Replace the queueing stub with the real registrar and drain whatever queued.
-- ⚠️ `ran` is keyed by MOD name; P.ports is keyed by PORT name and they are not
-- the same string. Testing `P.ports[mod_name]` here ran every queued mod's setup
-- twice (once draining the queue, once "relinking"), which in a mod that
-- registers anything non-idempotent would have been a real duplicate.
local ran = {}
local function run_setup(name, fn, why)
  P._mods[name] = fn
  P._queue[name] = nil
  if ran[name] then return end
  ran[name] = true
  local ok, err = pcall(fn, P)
  log("[port] mod '%s' setup %s%s", name, ok and "ok" or ("FAILED: " .. tostring(err)), why)
end

P.mod = function(name, fn) ran[name] = nil; run_setup(name, fn, "") end

for name, fn in pairs(P._queue) do run_setup(name, fn, " (queued)") end
-- A LIBRARY reload wipes P.ports; re-run every setup we have ever seen so the
-- ports come back without the mod file having to be touched.
for name, fn in pairs(P._mods) do run_setup(name, fn, " (relink)") end

log("[port] mhfu_port runtime ready")
