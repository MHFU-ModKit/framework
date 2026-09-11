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
--   moves = { charge = { main = 3, sub = 6, clip = "charge", after = "skid",
--                        hold_max = 8 },
--             skid   = { main = 0, sub = 3, clip = "stop" } }
--
-- `port:play("charge")` writes the behaviour pair AND latches the clip, so the
-- next executor dispatch is overridden to the port's own charge animation. The
-- host's opinion about which clip belongs to that move is discarded.
--
-- 🔴 A MOVE IS ONE LINK OF A CHAIN, AND A PAIR WRITTEN FROM HERE IS NOT WHAT THE
-- ENGINE WRITES. The engine enters a pair through its ENTER-ACTION (vt+0x88) and a
-- per-main translator that PROVISIONS the handler — the Tigrex charge gets its run
-- budget (+0x76C) there — and the handler hands to the next pair itself when that
-- budget is spent: (1,4) -> (0,3) -> (0,1). This library's act_set writes the two
-- bytes and nothing else, so a forced charge has no budget and PARKS in its last
-- phase, hitbox spent, for as long as it stands (measured 2026-09-11: 38 s, zero
-- spawns; the editor's Moves tab draws the chains, `species/em75.json` `next`).
-- So declare the chain: `after` is the move handed to when this one is over or
-- has stood `hold_max` ticks, and `play()` REFUSES to re-enter a pair that is
-- already running (`{force = true}` restarts it, for debugging; it is never the
-- right thing in a shipping mod). A move that neither ends nor declares `after`
-- is logged as parked after PARKED_TICKS.
--
-- 🟢 THE NATIVE SEAMS (em_vhook v3, issues #15/#16) FIX THE PROVISIONING GAP.
-- When the framework was built with `em_vhook` and it has latched the monster's
-- vtable (`mhfu.em_installed()`), this library stops writing the two cells:
--
--   play(name)   -> mhfu.em_request(main, sub): the pair is entered on the GAME
--                   THREAD inside the next AI frame through the engine's own
--                   enter-action, so the translator provisions it — a requested
--                   charge gets its run budget and hands to the skid by itself.
--   claim        -> mhfu.em_substitute: a move that CLAIMS host pairs (`claim =
--                   { main = 1 }` = every main-1 attack the host brain picks) is
--                   entered instead of them, before the species enter-action runs.
--                   The host's brain keeps deciding WHEN; the port decides WHAT.
--   port:rule{}  -> mhfu.em_rule: a 30 Hz trigger evaluated every frame in the
--                   slot-29 stub — "in (1,4) >= 15 frames, receding, d >= 250 ->
--                   lunge_stop" — with no Lua in the loop. Up to 4.
--
-- Without the seam (old PRX, mod off the manifest) everything below falls back
-- to the byte-writing act_set, `after`/`hold_max` end the pair from here, and
-- the log says so once.
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
-- 🔴 +0x2E4 is CURRENT HP (the framework's entity_hp; the player's is the same
-- offset). +0x41E is the MAX — the resolver's clamp. port_state.hp read +0x41E
-- until 2026-09-11 and printed a flat 3200 through a fight the HIT lines
-- counted down from.
local OFF_HP        = 0x2E4
local OFF_MAX_HP    = 0x41E
local OFF_HZ_STATE  = 0x481   -- s8, which hitzone grid state the damage math reads
local OFF_FLAGS     = 0x638   -- u32 flag word (0x8000 = visible; bit 0x20 <-> +0x481)
local OFF_FREEZE    = 0x4B8
-- The two cells that say WHO the monster has committed to, as opposed to
-- whether it has noticed anyone. See `targets_player` in the state table.
local OFF_TARGET    = 0x2F4   -- u32, resolved combat target pointer
local OFF_ACQUIRED  = 0x2A4   -- u8, aggro-eval target-acquired flag
local FREEZE_BITS   = 0x100 | 0x10000
-- a scripted move whose phase byte has not moved for this many ticks (2 Hz) and
-- that declares neither `after` nor `hold_max` is PARKED: logged once per play
local PARKED_TICKS  = 10

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

-- ------------------------------------------------------------- native seams
-- em_vhook v3 (framework/prx/mods/em_vhook, docs/EM_OVERLAY_ABI.md §15). The
-- bindings exist on a framework built with the mod; the seam is LIVE once the
-- mod has latched the spawned monster's vtable, which happens on the same spawn
-- event this library binds `port.ent` on — so the first tick after the spawn is
-- the earliest anything native can be declared, and `_arm_native` runs there.
local EM_ANY       = mhfu.EM_ANY or 0xFE
local EM_UNLIMITED = mhfu.EM_UNLIMITED or -1
local MAX_SUBS, MAX_RULES = 4, 4

local function native_ready()
  return mhfu.em_installed ~= nil and mhfu.em_installed() == true
end
P.native_ready = native_ready

--- The seam's counters, or nil without it. `ring` is the last 8 enter-action
--- dispatches the engine made (oldest first: main, sub, mode, subst) — the thing
--- to read when a request or a substitution did not land.
function P.native_status()
  if mhfu.em_status == nil then return nil end
  return mhfu.em_status()
end

local function ring_text(st)
  if not st or not st.ring then return "?" end
  local t = {}
  for _, e in ipairs(st.ring) do
    t[#t + 1] = string.format("(%d,%d,m%d)%s", e.main, e.sub, e.mode,
                              e.subst ~= 0 and "*" or "")
  end
  return table.concat(t, " ")
end

--- A move's `claim` as a main-state bitmask + sub: `{ main = 1 }`, `{ main = {0, 1} }`,
--- `{ main = 1, sub = 7 }`. Returns mask, sub (EM_ANY when none).
local function claim_of(mv)
  local c = mv.claim
  if c == nil then return nil end
  if type(c) == "number" then c = { main = c } end
  local mains = c.main
  if type(mains) == "number" then mains = { mains } end
  local mask = 0
  for _, m in ipairs(mains or {}) do mask = mask | (1 << m) end
  if mask == 0 then return nil end
  return mask, c.sub or EM_ANY
end

-- ------------------------------------------------------------- act_set
-- act_set (0x09AC8690) reimplemented with plain memory writes — THE FALLBACK,
-- used only while the native seam is not live (`native_ready()` false: an old
-- PRX, or em_vhook off the manifest). ⚠️ A pair written this way is NOT
-- PROVISIONED: the engine's own way in runs the per-main translator first
-- (the charge's run budget +0x76C is set there), and this skips it — so a
-- forced charge parks in its last phase with its hitbox spent, which is what
-- `after`/`hold_max` exist to end. The engine's version also clears the
-- per-slot cursors behind two condition checks; this does the unconditional
-- part, which is the minimum a handler needs to run from its first phase.
-- Measured: a pair written this way ran a complete attack and killed the hunter.
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
--           `claim = { main = 1 }` (or `{ main = {0, 1}, sub = 7 }`): every
--           enter-action the HOST BRAIN makes for a pair in that set is entered
--           as this move instead (the substitution seam). A move with a claim
--           is how a port with one attack rides the host's whole attack timing.
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
    _by_pair = {},      -- main*256+sub -> move name, for pairs the ENGINE enters
    _refused = {},      -- move name -> refusals, so the log says it once
    _rules  = {},       -- Port:rule{} declarations, installed by _arm_native
    _native_armed = false,
    _req    = nil,      -- a play() issued through the seam, until it lands
  }, Port)
  -- Declared pairs the engine enters ON ITS OWN get the port's clip too — the
  -- hook below paints them — so the declared mapping holds whether the brain or
  -- the script picked the move. First name wins for a pair declared twice.
  local names = {}
  for n in pairs(self.moves) do names[#names + 1] = n end
  table.sort(names)
  for _, n in ipairs(names) do
    local mv = self.moves[n]
    local k = (mv.main or 0) * 256 + (mv.sub or 0)
    if self._by_pair[k] == nil then self._by_pair[k] = n end
  end

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
---
--- 🔴 Refuses to enter a pair that is ALREADY RUNNING — ours or the engine's own.
--- act_set zeroes the phase cursor, so writing the pair he is in restarts the
--- action from phase 0: the clip from frame 0, the hitbox node (which spawns
--- once per ENTRY) again, and the engine's own walk (charge -> skid -> think)
--- cut short every time. A brain that wants "charge again" waits for the move to
--- END (`s.move == nil`) and plays it then. `opts.force = true` restarts anyway,
--- for a debugging session; a shipping mod has no use for it. `opts.raw = true`
--- writes the cells by hand even when the native seam is live (debugging the
--- seam itself; nothing else wants an unprovisioned pair).
function Port:play(move_name, min_gap, opts)
  local mv = self.moves[move_name]
  if not mv then log("[port:%s] no such move '%s'", self.name, tostring(move_name)); return false end
  if self.ent == 0 then return false end
  min_gap = min_gap or 2
  if self._played == move_name and (self._tick - (self._played_at or -99)) < min_gap then
    return false
  end
  local live_main, live_sub = mhfu.read_u8(self.ent + OFF_MAIN), mhfu.read_u8(self.ent + OFF_SUB)
  if live_main == mv.main and live_sub == mv.sub and not (opts and opts.force) then
    local n = (self._refused[move_name] or 0) + 1
    self._refused[move_name] = n
    if n == 1 or n % 20 == 0 then
      log("[port:%s] play('%s') refused: (%d,%d) is already running%s — writing it "
          .. "again restarts it from phase 0 (the hitbox spawns once per ENTRY). Let it "
          .. "end (after=/hold_max=), or play(name, gap, {force=true}) for a debug restart.%s",
          self.name, move_name, mv.main, mv.sub,
          self.move == move_name and " (ours)" or " (the engine's own)",
          n > 1 and string.format(" [x%d]", n) or "")
    end
    return false
  end
  self.move, self._played, self._played_at = move_name, move_name, self._tick
  self._phase_seen, self._phase_ticks, self._parked = nil, 0, false
  self._entered = { mv.main, mv.sub }
  self.clip = mv.clip and self.clips[mv.clip] or mv.anim
  -- 🟢 THE PROVISIONED WAY IN. With the seam live the pair is not written from
  -- here at all: the slot-29 stub issues the engine's own enter-action on the
  -- next AI frame (<= 33 ms), the translator provisions the handler, act_set
  -- writes the cells. The clip latch above is set FIRST because the executor
  -- dispatch that opens the move happens on the game thread inside that same
  -- frame. The landing is checked on the next tick (`_req`).
  if native_ready() and not (opts and opts.raw) then
    local st = P.native_status()
    self._req = { name = move_name, at = self._tick, done = st and st.req_done or 0,
                  main = mv.main, sub = mv.sub }
    mhfu.em_request(mv.main, mv.sub, mv.mode or 0)
  else
    if not P._once.raw_warned and mhfu.em_installed == nil then
      P._once.raw_warned = true
      log("[port] this framework has no em_vhook seam: pairs are written by hand "
          .. "(unprovisioned — a forced charge parks; after=/hold_max= end it)")
    end
    act_set(self.ent, mv.main, mv.sub)
  end
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

--- The declared chain: `mv.after`, entered now. Returns true when a next link
--- was taken. If the engine already moved him INTO that pair (its own hand-off
--- landed where the declaration points) the move is adopted rather than
--- re-written — writing it would restart what the engine just started.
function Port:_walk_after(mv, ended, why)
  local nxt = mv.after
  if not nxt then return false end
  local nm = self.moves[nxt]
  if not nm then
    log("[port:%s] move '%s' declares after='%s', which is not a declared move",
        self.name, ended, tostring(nxt))
    return false
  end
  local lm, ls = mhfu.read_u8(self.ent + OFF_MAIN), mhfu.read_u8(self.ent + OFF_SUB)
  if lm == nm.main and ls == nm.sub then
    self.move, self._played, self._played_at = nxt, nxt, self._tick
    self._phase_seen, self._phase_ticks, self._parked = nil, 0, false
    self._entered = { nm.main, nm.sub }
    self.clip = nm.clip and self.clips[nm.clip] or nm.anim
    self._clip_uses = nm.latch or 1
    log("[port:%s] '%s' %s -> after='%s': the engine is already in (%d,%d), adopted",
        self.name, ended, why, nxt, lm, ls)
    return true
  end
  if self:play(nxt) then
    log("[port:%s] '%s' %s -> after='%s' (%d,%d)", self.name, ended, why, nxt,
        nm.main, nm.sub)
    return true
  end
  return false
end

--- Hand both channels back to the engine's own AI.
function Port:release()
  self.move, self.clip, self._played, self._clip_uses = nil, nil, nil, 0
  self._req, self._entered = nil, nil
  self:unpin()
end

--- A native 30 Hz brain rule (the slot-29 seam; issue #16). Evaluated every
--- frame by the stub, no Lua in the loop:
---
---   port:rule{ from = "lunge",            -- a move name, or { main = 1, sub = 4 },
---              from_main = { 0, 1 },      -- ...or a set of main states (any sub)
---              min_frames = 15,           -- the pair must have stood this long
---              dist = { 250, 1e9 },       -- player XZ distance window [lo, hi)
---              receding = true,           -- only while the gap is GROWING
---              closing = false,           -- only while it is SHRINKING
---              play = "lunge_stop",       -- the move to enter (its main, sub)
---              mode = 0, cooldown = 30, count = mhfu.EM_UNLIMITED }
---
--- Fires at most once per entry into `from` (the pair changes when it fires),
--- then `cooldown` frames must pass. Up to 4 rules per port; declared in the
--- mod's setup and installed when the seam is live. Without the seam the rule
--- is inert and logged as such — the 2 Hz brain is the fallback.
function Port:rule(spec)
  if #self._rules >= MAX_RULES then
    log("[port:%s] rule ignored: the seam holds %d", self.name, MAX_RULES)
    return self
  end
  local r = { mask = 0, sub = EM_ANY, min_frames = spec.min_frames or 0,
              dist_lo = spec.dist and spec.dist[1] or 0,
              dist_hi = spec.dist and spec.dist[2] or 1.0e9,
              receding = spec.receding and true or false,
              closing = spec.closing and true or false,
              mode = spec.mode or 0, cooldown = spec.cooldown or 0,
              count = spec.count or EM_UNLIMITED, play = spec.play, label = spec.label }
  local from = spec.from
  if type(from) == "string" then
    local mv = self.moves[from]
    if not mv then log("[port:%s] rule: no such move '%s'", self.name, from); return self end
    r.mask, r.sub = 1 << mv.main, mv.sub
  elseif type(from) == "table" then
    r.mask, r.sub = 1 << (from.main or 0), from.sub or EM_ANY
  end
  local fm = spec.from_main
  if type(fm) == "number" then fm = { fm } end
  for _, m in ipairs(fm or {}) do r.mask = r.mask | (1 << m) end
  local to = self.moves[spec.play or ""]
  if not to then
    log("[port:%s] rule: play='%s' is not a declared move", self.name, tostring(spec.play))
    return self
  end
  r.to_main, r.to_sub = to.main, to.sub
  if r.mask == 0 then
    log("[port:%s] rule -> '%s': no `from` pair, ignored", self.name, spec.play)
    return self
  end
  self._rules[#self._rules + 1] = r
  self._native_armed = false          -- (re)install on the next tick
  return self
end

--- Install every claim and rule on the seam. Runs on the first tick the seam
--- is live for this port, and again after a redefine (hot reload). Idempotent:
--- the table is rewritten slot by slot, unused slots cleared.
function Port:_arm_native()
  self._native_armed = true
  local names = {}
  for n, mv in pairs(self.moves) do if claim_of(mv) then names[#names + 1] = n end end
  table.sort(names)
  local slot = 0
  for _, n in ipairs(names) do
    local mv = self.moves[n]
    local mask, sub = claim_of(mv)
    if slot < MAX_SUBS then
      mhfu.em_substitute(slot, mask, sub, mv.main, mv.sub, EM_UNLIMITED)
      log("[port:%s] claim: host enter-actions with main in 0x%02X%s -> '%s' (%d,%d), standing",
          self.name, mask, sub == EM_ANY and "" or string.format(" sub %d", sub),
          n, mv.main, mv.sub)
    else
      log("[port:%s] claim on '%s' ignored: the seam holds %d", self.name, n, MAX_SUBS)
    end
    slot = slot + 1
  end
  for k = slot, MAX_SUBS - 1 do mhfu.em_substitute(k, 0, EM_ANY, 0, 0, 0) end
  for i = 1, MAX_RULES do
    local r = self._rules[i]
    if r then
      mhfu.em_rule(i - 1, { from_mask = r.mask, from_sub = r.sub, to_main = r.to_main,
                            to_sub = r.to_sub, mode = r.mode, min_frames = r.min_frames,
                            dist_lo = r.dist_lo, dist_hi = r.dist_hi, receding = r.receding,
                            closing = r.closing, cooldown = r.cooldown, count = r.count })
      log("[port:%s] rule %d: main 0x%02X%s >=%d frames d[%d,%s)%s%s -> '%s' (%d,%d)",
          self.name, i, r.mask, r.sub == EM_ANY and "" or (" sub " .. r.sub),
          r.min_frames, math.floor(r.dist_lo),
          r.dist_hi >= 1e9 and "inf" or tostring(math.floor(r.dist_hi)),
          r.receding and " receding" or "", r.closing and " closing" or "",
          r.play, r.to_main, r.to_sub)
    else
      mhfu.em_rule(i - 1, nil)
    end
  end
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
--
-- THE ATTACK SIDE (issue #33) — where he hits YOU — is the same shape in the
-- other direction. A handler spawns an attack by id; the 0x18 record at
-- `attack_tables.records + id*0x18` names a volume SET (+0x0A), and the set is
-- the same 0x28 records as above, reached through the overlay's own pointer
-- table `attack_tables.volumes + set*4`, walked to the sentinel by 0x09C42650.
-- Both addresses are the species overlay's, static, exported from
-- species/emNN.json (em75: 0x09D60768 / 0x09D60848). Each authored set goes IN
-- PLACE over the host's set of that index, then a sentinel; each `attacks` entry
-- writes only the levers it names (+0x02 power, +0x09 element, +0x0A volume).
-- ⚠️ These are absolute addresses, not a pointer the engine handed us, so a set
-- is written ONLY if its live record count equals the exported `cap` on first
-- contact — otherwise the table is not what the export assumed and it is left
-- alone, with a log line. Set replacement proven by RAM poke on a native Tigrex
-- (645 -> 152 -> 1381 units); this Lua path validated live 2026-09-11 (hot-reloaded
-- into a running quest: 1 attack set(s)/10 volume(s) applied, the ported Zinogre's
-- lunge connected through the authored sphere, the power lever took).
local SPECIES_TABLE  = 0x09BB87C0
local SPECIES_STRIDE = 0x1D0
local F_SPHERES      = 0x240
local F_STATES       = 0x2FC
local REC            = 0x28
local GRID_BLOCK     = 0x48
local GRID_ROWS, GRID_COLS = 7, 10
local SENTINEL       = 0xFFFF
local MAX_RECORDS    = 512    -- a walk bound; the longest set in the game is 49
local ATK_REC        = 0x18   -- one attack record
local ATK_POWER, ATK_ELEMENT, ATK_VOLUME = 0x02, 0x09, 0x0A   -- the measured levers

P._hit = P._hit or {}         -- port name -> table, from the generated module

--- Register a port's hit tables. `tbl` = { species, id, volumes = {{bone, shape,
--- row, part, flags, radius, ax, ay, az, bx, by, bz}, ...} | nil, grid = {{row*7}
--- x states} | nil }. Keyed by PORT name so the data module and the brain module
--- can load in either order; the tick joins them.
function P.hit(name, tbl)
  P._hit[name] = tbl
  local port = P.ports[name]
  if port then port._hit_id = nil end       -- a re-export applies on the next tick
  local nsets = 0
  if tbl.attack_sets then for _ in pairs(tbl.attack_sets) do nsets = nsets + 1 end end
  log("[port:%s] hit tables registered: %s volume(s), %s grid state(s), %d attack "
      .. "set(s), %s attack record(s), id %s",
      name, tbl.volumes and #tbl.volumes or "no", tbl.grid and #tbl.grid or "no",
      nsets, tbl.attacks and #tbl.attacks or "no", tostring(tbl.id))
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

--- The live address of attack volume set `set`, through the overlay's pointer
--- table, or nil (+why) when the export names a set the table does not have.
local function attack_set_base(tbl, set)
  local at = tbl.attack_tables
  if not at or not at.volumes then return nil, "no attack_tables in the export" end
  if at.n_sets and set >= at.n_sets then
    return nil, string.format("set %d but the host has %d", set, at.n_sets)
  end
  local base = mhfu.read_u32(at.volumes + set * 4)
  if base == 0 or not mhfu.mem_valid(base) then
    return nil, string.format("set %d pointer 0x%08X invalid", set, base)
  end
  return base
end

--- Sorted set indices, so the log and the writes are in one order every time.
local function attack_set_indices(tbl)
  local idx = {}
  if tbl.attack_sets then
    for k in pairs(tbl.attack_sets) do idx[#idx + 1] = k end
  end
  table.sort(idx)
  return idx
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
  for _, set in ipairs(attack_set_indices(tbl)) do
    local spec = tbl.attack_sets[set]
    local base = attack_set_base(tbl, set)
    if not base then return false end
    local cap = (port._atk_caps and port._atk_caps[set]) or spec.cap or #spec.volumes
    local n = math.min(#spec.volumes, cap)
    if n == 0 then
      if mhfu.read_u16(base) ~= SENTINEL then return false end
    else
      local r = spec.volumes[1]
      if mhfu.read_u16(base) ~= r[1] then return false end
      if math.abs(rf(base + 0x0C) - r[6]) > 0.01 then return false end
      if mhfu.read_u16(base + n * REC) ~= SENTINEL then return false end
    end
  end
  if tbl.attacks and tbl.attack_tables and tbl.attack_tables.records then
    for _, a in ipairs(tbl.attacks) do
      local rec = tbl.attack_tables.records + a.id * ATK_REC
      if a.power and mhfu.read_u8(rec + ATK_POWER) ~= a.power then return false end
      if a.element and mhfu.read_u8(rec + ATK_ELEMENT) ~= a.element then return false end
      if a.volume and mhfu.read_u8(rec + ATK_VOLUME) ~= a.volume then return false end
    end
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
  -- the attack sets: each in place over the host's set of that index. The live
  -- count is measured ONCE per set (P._once, for the same reload reason as the
  -- hurtbox cap) and must equal the exported `cap`, or the whole apply fails
  -- before anything is written: the address is static, and a wrong count means
  -- the overlay in RAM is not the one the export was built against.
  local sets = attack_set_indices(tbl)
  if #sets > 0 then
    port._atk_caps = port._atk_caps or {}
    for _, set in ipairs(sets) do
      local spec = tbl.attack_sets[set]
      local base, why = attack_set_base(tbl, set)
      if not base then return false, "attack " .. why end
      local capkey = string.format("atkcap:%d:%d", port.species, set)
      if not P._once[capkey] then
        local n = 0
        while n < MAX_RECORDS and mhfu.read_u16(base + n * REC) ~= SENTINEL do
          n = n + 1
        end
        if spec.cap and n ~= spec.cap then
          return false, string.format(
            "attack set %d @0x%08X holds %d record(s), the export expected %d — "
            .. "not the table the export was built against; NOT written",
            set, base, n, spec.cap)
        end
        P._once[capkey] = n
        log("[port:%s] attack set %d @0x%08X holds %d record(s) — the in-place cap",
            port.name, set, base, n)
      end
      port._atk_caps[set] = P._once[capkey]
    end
    local n_sets, n_vols = 0, 0
    for _, set in ipairs(sets) do
      local spec = tbl.attack_sets[set]
      local base = attack_set_base(tbl, set)
      local cap = port._atk_caps[set]
      local n = #spec.volumes
      if n > cap then
        log("[port:%s] ⚠️ attack set %d: %d volume(s) but only %d fit in place — "
            .. "TRUNCATED", port.name, set, n, cap)
        n = cap
      end
      for i = 1, n do write_record(base + (i - 1) * REC, spec.volumes[i]) end
      write_sentinel(base + n * REC)
      n_sets, n_vols = n_sets + 1, n_vols + n
    end
    wrote[#wrote + 1] = string.format("%d attack set(s)/%d volume(s) via 0x%08X",
                                      n_sets, n_vols, tbl.attack_tables.volumes)
  end
  if tbl.attacks and #tbl.attacks > 0 then
    local at = tbl.attack_tables
    if not at or not at.records then
      return false, "attacks but no attack_tables.records in the export"
    end
    local n = 0
    for _, a in ipairs(tbl.attacks) do
      if at.n_records and a.id >= at.n_records then
        log("[port:%s] ⚠️ attack record %d but the host has %d — skipped",
            port.name, a.id, at.n_records)
      else
        local rec = at.records + a.id * ATK_REC
        if a.power   then mhfu.write_u8(rec + ATK_POWER,   a.power)   end
        if a.element then mhfu.write_u8(rec + ATK_ELEMENT, a.element) end
        if a.volume  then mhfu.write_u8(rec + ATK_VOLUME,  a.volume)  end
        n = n + 1
      end
    end
    wrote[#wrote + 1] = string.format("%d attack record(s) @0x%08X", n, at.records)
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

-- Every hit the port takes, with the amount: the number issue #19 is measured
-- in. Fires from the 5 Hz monster poll (an HP-drop edge), so two hits inside
-- 200 ms arrive as one line with their sum. `st` is entity+0x481, the grid
-- STATE the hit was multiplied through (measured 2026-09-11: it follows bit
-- 0x20 of +0x638 — set = state 1, clear = state 0 — and flips mid-fight), and
-- `f638` the flag word, so a damage number can be read against the right row.
-- Registered OUTSIDE the once-block on purpose: lb_on_damaged installs its C
-- trampoline once and store_ref() replaces the closure, so a library reload
-- picks up a new format here without stacking handlers.
mhfu.on_bigmonster_damaged(function(ent, mtype, amount, hp, slot)
  for _, port in pairs(P.ports) do
    if port.ent == ent then
      port._hits = (port._hits or 0) + 1
      log("[port:%s] HIT #%d  -%d  hp=%d  st=%d f638=0x%X", port.name, port._hits,
          amount, hp, mhfu.read_u8(ent + OFF_HZ_STATE), mhfu.read_u32(ent + OFF_FLAGS))
    end
  end
end)

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
    if ctx.entity == port.ent then
      if port.clip and (port._clip_uses or 0) > 0 then
        port._clip_uses = port._clip_uses - 1
        if port.clip ~= ctx.action_id then
          log("[port:%s] clip %d -> %d  (move=%s)", port.name, ctx.action_id,
              port.clip, tostring(port.move))
        end
        return port.clip
      end
      -- No scripted move: is the pair the ENGINE put him in a declared one? Then
      -- the declared clip paints it, for as many dispatches as its latch says —
      -- the mapping is a fact about the pair, not about who entered it. Nothing
      -- else changes: no `move`, no `after`, the engine walks its own chain.
      if port.ent ~= 0 and next(port._by_pair) ~= nil then
        local k = mhfu.read_u8(port.ent + OFF_MAIN) * 256 + mhfu.read_u8(port.ent + OFF_SUB)
        local name = port._by_pair[k]
        local mv = name and port.moves[name]
        local clip = mv and (mv.clip and port.clips[mv.clip] or mv.anim)
        if clip then
          local paint = port._paint
          if not paint or paint.key ~= k then
            paint = { key = k, uses = mv.latch or 1 }
            port._paint = paint
          end
          if paint.uses > 0 then
            paint.uses = paint.uses - 1
            if clip ~= ctx.action_id then
              log("[port:%s] clip %d -> %d  (engine entered '%s' itself)", port.name,
                  ctx.action_id, clip, name)
            end
            return clip
          end
        end
      end
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
    phase = mhfu.read_u8(ent + OFF_PHASE),
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
    -- true while the em_vhook seam drives play()/claims/rules (see the header)
    native = native_ready(),
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
    max_hp = mhfu.read_u16(ent + OFF_MAX_HP),
    hz_state = mhfu.read_u8(ent + OFF_HZ_STATE),
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
      -- the hook's paint of an engine-entered pair is per ENTRY: forget it once
      -- he has left that pair, so his next native charge gets painted too
      if port._paint then
        local k = mhfu.read_u8(port.ent + OFF_MAIN) * 256 + mhfu.read_u8(port.ent + OFF_SUB)
        if k ~= port._paint.key then port._paint = nil end
      end
      -- the seam comes up on the spawn event; the first tick after it is where
      -- the claims and rules go in (and again after a redefine)
      if not port._native_armed and native_ready() then
        if not P._once["native:" .. port.name] then
          P._once["native:" .. port.name] = true
          log("[port:%s] native seam live: play() enters through the engine's "
              .. "enter-action (provisioned); claims and rules installed", port.name)
        end
        port:_arm_native()
      end
      -- a play() issued through the seam: did the engine take it?
      if port._req then
        local rq, st = port._req, P.native_status()
        local lm, ls = mhfu.read_u8(port.ent + OFF_MAIN), mhfu.read_u8(port.ent + OFF_SUB)
        if lm == rq.main and ls == rq.sub then
          log("[port:%s] '%s' entered natively (%d,%d), provisioned", port.name, rq.name, lm, ls)
          port._req = nil
        elseif ls == rq.sub then
          -- the translator's alternative main (em75 id 4: (1,4) or (2,4))
          log("[port:%s] '%s' entered natively as (%d,%d) — the translator's main for "
              .. "id %d; tracking that pair", port.name, rq.name, lm, ls, rq.sub)
          port._entered = { lm, ls }
          port._req = nil
        elseif st and st.req_done > rq.done and g_tick - rq.at >= 1
               and st.req_main == rq.main and st.req_sub == rq.sub then
          -- it LANDED (the cells read our pair right after the call) and the
          -- pair is already over: a charge the 30 Hz rule ended inside one
          -- tick reads exactly like this. Let the ended path below handle it —
          -- it adopts `after` if the engine is standing there, else plays it.
          log("[port:%s] '%s' entered natively (%d,%d) and was over within the tick "
              .. "(now (%d,%d))", port.name, rq.name, rq.main, rq.sub, lm, ls)
          port._req = nil
        elseif st and st.req_done > rq.done and g_tick - rq.at >= 1 then
          -- issued (req_done moved) but the cells never showed it: the enter-
          -- action declined it, or something re-entered in the same frame
          log("[port:%s] '%s' (%d,%d) was requested and issued but the cells read "
              .. "(%d,%d) — result after the call (%d,%d); last enter-actions: %s",
              port.name, rq.name, rq.main, rq.sub, lm, ls, st.req_main, st.req_sub,
              ring_text(st))
          port._req, port.move, port.clip, port._clip_uses = nil, nil, nil, 0
          port._entered = nil
        elseif g_tick - rq.at >= 3 then
          log("[port:%s] '%s' request never issued after %d ticks (req_done %s) — "
              .. "is the seam still latched?", port.name, rq.name, g_tick - rq.at,
              tostring(st and st.req_done))
          port._req, port.move, port.clip, port._clip_uses = nil, nil, nil, 0
          port._entered = nil
        end
      end
      if port.move then
        local mv = port.moves[port.move]
        local lm, ls = mhfu.read_u8(port.ent + OFF_MAIN), mhfu.read_u8(port.ent + OFF_SUB)
        local held = g_tick - (port._played_at or g_tick)
        local em, es = mv and mv.main, mv and mv.sub
        if port._entered then em, es = port._entered[1], port._entered[2] end
        if port._req then
          -- not landed yet (<= 1 tick): nothing to judge
        elseif mv and (lm ~= em or ls ~= es) then
          log("[port:%s] move '%s' (%d,%d) ended after %d ticks -> (%d,%d)", port.name,
              port.move, em, es, held, lm, ls)
          local ended = port.move
          port.last_move, port.last_move_ticks = ended, held
          port.move, port.clip, port._clip_uses, port._entered = nil, nil, 0, nil
          port:_walk_after(mv, ended, "ended")
        elseif mv then
          -- still standing. `hold_max` ends it from here; otherwise watch the
          -- phase byte, because a pair whose phase stops moving has nothing
          -- left to do and will not leave by itself.
          if mv.hold_max and held >= mv.hold_max then
            log("[port:%s] move '%s' held %d ticks (hold_max %d) on (%d,%d)", port.name,
                port.move, held, mv.hold_max, lm, ls)
            local ended = port.move
            port.last_move, port.last_move_ticks = ended, held
            port.move, port.clip, port._clip_uses, port._entered = nil, nil, 0, nil
            if not port:_walk_after(mv, ended, "hold_max") then port:release() end
          elseif not mv.after then
            local phase = mhfu.read_u8(port.ent + OFF_PHASE)
            if phase == port._phase_seen then
              port._phase_ticks = (port._phase_ticks or 0) + 1
            else
              port._phase_seen, port._phase_ticks = phase, 0
            end
            if port._phase_ticks >= PARKED_TICKS and not port._parked then
              port._parked = true
              log("[port:%s] move '%s' PARKED: (%d,%d) phase %d unchanged for %d ticks. "
                  .. "A parked pair spawns nothing more — the engine provisions this pair "
                  .. "on its own way in (a run budget, a target) and act_set does not. "
                  .. "Declare after=/hold_max= on the move, or release().",
                  port.name, port.move, lm, ls, phase, port._phase_ticks)
            end
          end
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
