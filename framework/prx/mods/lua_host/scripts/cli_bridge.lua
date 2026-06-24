-- cli_bridge.lua — the in-game side of the MHFU debug CLI's PRX bridge.
--
-- The CLI (python -m mhfu_bot.cli.shell) writes a small command block in extra
-- RAM; this script polls it each tick and applies at-speed holds that a pure
-- debugger can't do smoothly: force a big monster's ACTION (held coherently via
-- on_bigmonster_action), freeze/unfreeze its AI, and clear. See
-- specs/003-debug-cli/tasks.md for the block layout + commands.
--
-- Deploy: copy to ms0:/PSP/PLUGINS/mhfu_framework/mods/cli_bridge.lua and
-- hot-reload (edit-on-memstick, ~0.5s) or cold boot. Needs memory=64 (the block
-- lives at 0x0A7F0000 in the extra RAM) and a big monster loaded.

local BR        = 0x0A7F0000          -- bridge block base
local MAGIC     = 0x4D484252          -- 'MHBR'
local FREEZE    = 0x100 | 0x10000     -- entity+0x4B8 bits that halt the AI tick
local OFF_GATE  = 0x4B8

local CMD_FORCE = 1
local CMD_FREEZE = 2
local CMD_CLEAR = 3

local last_seq   = -1
local g_force    = nil                 -- forced action id, or nil
local g_freeze   = false
local g_slot     = 1                   -- target registry slot

-- Priority-chain action override: while a force is active, every big-monster
-- action decision returns OUR id, so the engine fans it coherently to all body
-- slots (no desync). nil -> pass through the engine's own choice.
mhfu.on_bigmonster_action(function(ctx)
  if g_force ~= nil then return g_force end
  return ctx.action_id
end, 90)

local function target_entity()
  local e = mhfu.entity_at(g_slot)
  if e and e ~= 0 then return e end
  return nil
end

-- global (not local) so the chained wrapper resolves the FRESH definition after a
-- hot-reload instead of a stale captured upvalue
function cli_bridge_tick()
  -- poll the command block (only act on a new seq)
  if mhfu.read_u32(BR) == MAGIC then
    local seq = mhfu.read_u32(BR + 0x04)
    if seq ~= last_seq then
      last_seq = seq
      local cmd = mhfu.read_u32(BR + 0x08)
      local a0  = mhfu.read_u32(BR + 0x0C)   -- slot
      local a1  = mhfu.read_u32(BR + 0x10)   -- action id / on-off
      g_slot = a0
      if cmd == CMD_FORCE then
        g_force = a1
      elseif cmd == CMD_FREEZE then
        g_freeze = (a1 ~= 0)
      elseif cmd == CMD_CLEAR then
        g_force = nil
        g_freeze = false
      end
      mhfu.write_u32(BR + 0x18, seq)          -- ack
      mhfu.log(string.format("[cli_bridge] cmd=%d slot=%d a1=%d (seq=%d)", cmd, a0, a1, seq))
    end
  end

  local ent = target_entity()
  if not ent then return end

  if g_force ~= nil then
    -- keep the AI tick alive so the forced action keeps (re)playing = a loop
    local f = mhfu.read_u32(ent + OFF_GATE)
    if (f & FREEZE) ~= 0 then mhfu.write_u32(ent + OFF_GATE, f & ~FREEZE) end
    mhfu.write_u32(BR + 0x1C, g_force)        -- status: action being held
  elseif g_freeze then
    -- halt the AI tick -> the monster holds its current pose
    local f = mhfu.read_u32(ent + OFF_GATE)
    if (f & FREEZE) ~= FREEZE then mhfu.write_u32(ent + OFF_GATE, f | FREEZE) end
  end
end

-- The lua_host calls a single global `mhfu_tick`. CHAIN onto any tick another
-- script (e.g. brute_tigrex.lua) already installed instead of clobbering it, so
-- the bridge coexists with the Brute loader. Guarded against re-wrap on hot-reload
-- (cli_bridge_tick is redefined fresh each reload; the wrapper stays). Load order
-- is alphabetical, so "cli_bridge" wraps "brute_tigrex" naturally; if brute_tigrex
-- is later hot-reloaded it reclaims mhfu_tick — re-save cli_bridge.lua to re-wrap.
if not _G.__cli_bridge_installed then
  _G.__cli_bridge_prev_tick = rawget(_G, "mhfu_tick")
  _G.__cli_bridge_installed = true
  function mhfu_tick()
    local p = _G.__cli_bridge_prev_tick
    if p then pcall(p) end
    cli_bridge_tick()
  end
end

mhfu.log("[cli_bridge] ready — polling 0x0A7F0000 for CLI commands")
