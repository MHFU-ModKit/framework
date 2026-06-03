-- tigrex_spin.lua — COHERENT big-monster action force via on_bigmonster_action.
--
-- New seam (RE'd 2026-06-03): the action EXECUTOR 0x09AC5228 takes a1 = action
-- id and fans it to all 3 body-part slots itself, through the engine's own
-- vt[8] resolver + applier. Overriding a1 at its entry forces a coherent
-- whole-body action — no per-slot desync, no crash, and the engine's aggro/
-- music/stamina logic keeps running normally. This REPLACES the old per-slot
-- action_input force (which only saw slot 1 → body desync → crash).
--
-- a1 (action id) relates to the documented vt8_input as:
--   a1 = vt8_input(slot2) - 0x578   (= vt8_input(slot0) - 0x3E8)
--
-- HOT-RELOAD: edit on the memstick, ~0.5s, no cold boot. Change FORCE below.
--
-- Test plan: cold boot into the Giadrome quest (swap makes him a Tigrex),
-- find him; with FORCE on he should LOCK to the chosen action (default a safe
-- IDLE) the moment his AI ticks — coherent, no crash. Then try ANGRY_SPIN.

------------------------------------------------------------------------ CONFIG
local FORCE        = true          -- master enable
local FORCE_ACTION = "ANGRY_SPIN"  -- which action to lock (see ACTIONS below)
local ENGAGED_ONLY = true          -- only force once engaged (+0x05DC != 0), so
                                   -- he can roam->render first (visual bug bypass)
local TIGREX_SIZE  = 0.5
--------------------------------------------------------------------------------

-- action id (a1) for each named action = its slot2 vt8_input - 0x578.
local ACTIONS = {
  IDLE_STAND       = 0x20,   -- 0x0598
  IDLE_WALK        = 0x03,   -- 0x057B
  IDLE_TURN_LEFT   = 0x06,   -- 0x057E
  ANGRY_CHARGE     = 0x11,   -- 0x0589
  ANGRY_SPIN       = 0x2B,   -- 0x05A3  (the crash-prone one — try last)
}

local MON_GIADROME = mhfu.MON_GIADROME
local MON_TIGREX   = mhfu.MON_TIGREX
local FORCE_ID     = ACTIONS[FORCE_ACTION] or ACTIONS.IDLE_STAND

local seen = {}   -- dedupe the per-action log
local n    = 0

-- Swap Giadrome -> Tigrex before the loading-screen model load.
mhfu.on_quest_targets_building(function(quest)
  if quest == 0 then return end
  if mhfu.quest_has(quest, MON_GIADROME)
     and mhfu.quest_replace_monster(quest, MON_GIADROME, MON_TIGREX) then
    mhfu.log("[tigrex_spin] giadrome -> tigrex (native coords)")
  end
end)

mhfu.on_bigmonster_spawn(function(ent, mtype, slot, hp)
  if mtype ~= MON_TIGREX then return end
  mhfu.entity_set_size(ent, TIGREX_SIZE)
  seen = {}
  mhfu.log(string.format("[tigrex_spin] tigrex spawn ent=0x%08X hp=%d (0.5x)", ent, hp))
end)

-- THE coherent force: rewrite the action id the executor is about to run.
-- ctx = { entity, type, action_id }. Return a new action id (or ctx.action_id
-- to abstain). Engine fans it to all body slots.
mhfu.on_bigmonster_action(function(ctx)
  if ctx.type ~= MON_TIGREX then return ctx.action_id end

  -- Log each distinct engine pick (so we see his natural repertoire).
  if not seen[ctx.action_id] then
    seen[ctx.action_id] = true
    mhfu.log(string.format("[tigrex_spin] engine action_id=0x%02X (slot2 input 0x%04X)",
      ctx.action_id, (ctx.action_id + 0x578) & 0xFFFF))
  end

  if not FORCE then return ctx.action_id end

  -- Gate on the engage flag so he roams + renders first; only lock the spin
  -- once he's actually engaged (near + aggro'd) — avoids the pre-roam brick.
  if ENGAGED_ONLY and mhfu.read_u32(ctx.entity + 0x05DC) == 0 then
    return ctx.action_id
  end

  -- Clear the freeze gate (+0x4B8): forcing repeat-fire makes the engine OR in
  -- 0x100|0x10000 and halt the AI tick (§32j). Zero it each fire to stay alive.
  mhfu.write_u32(ctx.entity + 0x4B8, 0)

  -- Force the chosen action. Throttle the confirmation log.
  n = n + 1
  if n % 30 == 1 then
    mhfu.log(string.format("[tigrex_spin] forcing %s (a1=0x%02X), engine wanted 0x%02X",
      FORCE_ACTION, FORCE_ID, ctx.action_id))
  end
  return FORCE_ID
end, 100)

-- Auto-paint bosses (EU CWCheat _L 0x008B3A6A 0xFF -> real 0x090B3A6A).
-- Continuous 2 Hz re-write so the big monster shows on the map like a paintball.
local diag_n, last_patch, last_area = 0, -1, -1
function mhfu_tick()
  mhfu.write_u8(0x090B3A6A, 0xFF)

  -- Edge-log the executor patch word: catch the exact moment 0x09AC5228 flips
  -- (our J <-> original 0x27BDFFC0 <-> JIT marker 0x68xxxxxx), with screen/area,
  -- so we see what transition wipes the patch and whether re-patch snaps it back.
  diag_n = diag_n + 1
  local patch = mhfu.read_u32(0x09AC5228)
  local area  = mhfu.get_area_index()
  local heartbeat = (diag_n % 8 == 0)        -- ~4 s
  if patch ~= last_patch or area ~= last_area or heartbeat then
    local tag = "?"
    if patch == 0x27BDFFC0 then tag = "ORIGINAL(unhooked)"
    elseif (patch >> 26) == 0x02 then tag = "OUR-J(hooked)"
    elseif (patch >> 26) == 0x1A then tag = "JIT-MARKER" end
    local edge = (patch ~= last_patch) and " <<FLIP" or ""
    last_patch, last_area = patch, area
    local list = mhfu.entities_of_type(MON_TIGREX)
    local ent = list and list[1]
    local draw, eng, frz = 0, 0, 0
    if ent and ent ~= 0 then
      draw = mhfu.read_u32(ent + 0x008)
      eng  = mhfu.read_u32(ent + 0x05DC)
      frz  = mhfu.read_u32(ent + 0x4B8)
      -- Unfreeze net: the AI tick may be halted (executor not called), so the
      -- per-fire clear can't run — clear it here on the worker thread too.
      if frz ~= 0 then mhfu.write_u32(ent + 0x4B8, 0) end
    end
    mhfu.log(string.format(
      "[tigrex_spin] DIAG patch@5228=0x%08X %s scr=0x%02X area=%d draw=0x%08X eng=0x%08X frz=0x%08X%s",
      patch, tag, mhfu.get_screen_state(), area, draw, eng, frz, edge))
  end
end

mhfu.log(string.format("[tigrex_spin] registered (on_bigmonster_action) force=%s action=%s a1=0x%02X",
  tostring(FORCE), FORCE_ACTION, FORCE_ID))
