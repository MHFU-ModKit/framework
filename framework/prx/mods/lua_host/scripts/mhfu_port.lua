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
local FREEZE_BITS   = 0x100 | 0x10000

-- 🔴 The player's WORLD position is the combat entity's transform row 3.
-- `mhfu.player_pos()` reads 0x09998D50, which is the CAMERA EYE — it reported
-- d=26000 for a monster that was 365 units away. Framework bug; fixing the
-- header needs a PRX rebuild and there is no PSP toolchain on this machine, so
-- the correct address is used directly here.
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
-- framework has no native-call binding and no PSP toolchain is installed to add
-- one. The engine's version also clears the per-slot cursors behind two
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
  return true
end

--- Hand both channels back to the engine's own AI.
function Port:release()
  self.move, self.clip, self._played = nil, nil, nil
  self:unpin()
end

--- Point the monster at (x, z). Writes +0x1F4 only, so the engine's own VFPU
--- rotator renders the turn — no moonwalk, no matrix fight.
function Port:face(x, z)
  if self.ent == 0 then return end
  local mx = rf(self.ent + OFF_POS)
  local mz = rf(self.ent + OFF_POS + 8)
  local hw = math.floor(-atan2(z - mz, x - mx) * 32768.0 / math.pi) % 65536
  mhfu.entity_set_yaw(self.ent, hw)
end

--- Lock the monster's XZ where it stands. The Y is left alone: the engine drives
--- it to the local floor every frame and fighting that makes a monster sink.
---
--- 🔴 ONLY PIN A BEHAVIOUR THAT IS ALREADY STATIONARY. This rewrites the position
--- at 2 Hz while the engine advances it every frame, so against a pursuit state
--- it is a tug of war the player can see: a play session logged `pin corrected`
--- of 526-646 units on EVERY tick, and on screen the monster slid forward and
--- snapped back twice a second. That is not the lock misbehaving, it is the
--- wrong behaviour pair underneath it — pick one the census reports as
--- HOLDS + STATIONARY (`tools/em_state_census.py`) and the pin has nothing to do.
---
--- ⚠️ It is also a per-tick write, and CLAUDE.md rule 8 says never maintain a big
--- monster per-tick. The rule is about UNCONDITIONAL maintenance of +0x29A /
--- +0x638 / size / the freeze gate; this is conditional and scoped to one brain
--- phase. The `pin corrected` number is the honest gauge: at 0 it is doing
--- nothing and can go.
function Port:pin()
  if self.ent == 0 or self.pinned then return end
  self.pinned = { rf(self.ent + OFF_POS),
                  rf(self.ent + OFF_POS + 4),
                  rf(self.ent + OFF_POS + 8) }
end
function Port:unpin() self.pinned = nil end

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

-- ------------------------------------------------------------- events
-- Registered ONCE, at library load, and they dispatch to whatever ports happen
-- to be defined at the time. That is what makes hot-reloading a mod file safe:
-- the mod's setup runs again, P.ports is rebuilt, and no handler is ever stacked.

if not P._once.events then
P._once.events = true

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
    if ctx.entity == port.ent and port.clip then
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
  local last = port._last
  local travelled = 0
  if last and last[3] == sec then
    travelled = math.sqrt((mx - last[1]) ^ 2 + (mz - last[2]) ^ 2)
  end
  port._last = { mx, mz, sec }
  return {
    travelled = travelled, move = port.move, pinned = (port.pinned ~= nil),
    since_play = g_tick - (port._played_at or -999),
    port = port, ent = ent, tick = g_tick,
    x = mx, y = my, z = mz, px = px, py = py, pz = pz,
    dist = math.sqrt((mx - px) ^ 2 + (mz - pz) ^ 2),
    section = sec, area = area, same_section = (sec == area),
    -- true on the first tick of a new frame: dist is readable, travelled is not
    reframed = (last == nil or last[3] ~= sec),
    -- ⚠️ NOT full combat mode. +0x5DC comes up when the monster has DETECTED you
    -- and is pursuing — the '!' over its head. Played by hand, a swapped Brute
    -- showed the '!' and pursued but the yellow eye never appeared next to the
    -- hunter's name: a swapped big monster detects and does not latch combat
    -- (agent_memory_map.md, aggro-commit — the swap leaves the combat target
    -- unwired and engage flickers 1->0->1). No read for the latched state is
    -- known. Gate on this and you are gating on "has noticed you".
    engaged = mhfu.entity_engaged(ent),
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
          log("[port:%s] move '%s' ended after %d ticks -> (%d,%d)", port.name,
              port.move, g_tick - (port._played_at or g_tick),
              mhfu.read_u8(port.ent + OFF_MAIN), mhfu.read_u8(port.ent + OFF_SUB))
          port.move, port.clip = nil, nil
        end
      end
      if port.pinned then
        local slip = port:hold_pin()
        if slip > 40 then log("[port:%s] pin corrected %d units t=%d", port.name, slip, g_tick) end
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
