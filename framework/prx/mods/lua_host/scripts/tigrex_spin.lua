-- tigrex_visible_swap.lua — fix the Giadrome->Tigrex swap render bug.
--
-- ROOT CAUSE (RE'd 2026-06-04, live): the engine's per-frame visibility gate at
-- EBOOT 0x09AC4960 sets entity+0x004 bit 0x4 ("skip draw") UNLESS
--   (A) entity+0x29A (the monster's tracked SECTION index) == player's section, AND
--   (B) entity+0x638 & 0x8000.
-- A Giadrome->Tigrex swap spawns him at native coords but never initialises his
-- section tracker +0x29A, so the gate culls him until his first roam (which sets
-- it). Setting +0x29A = player section while co-located makes the engine clear
-- bit 0x4 itself -> he renders, coherently, no race. Verified live: writing a
-- wrong section hid him + moved his map marker; writing player section restored
-- him instantly.
--
-- THE FIX: while the player is physically in the same section as the Tigrex
-- (small world-space XZ distance), force +0x29A = get_area_index() and ensure
-- +0x638 bit 0x8000. Co-location guarantees we write his TRUE section, so we
-- never drag him across sections; once visible the engine maintains it through
-- his own roams.
--
-- HOT-RELOAD: edit on the memstick, ~0.5s, no cold boot.

------------------------------------------------------------------------ CONFIG
local TIGREX_SIZE     = 0.5
local COLOC_DIST      = 6000.0    -- world units; a snow section spans ~4000x3000
local PLAYER_POS_ADDR = 0x09998D50  -- camera target == player world pos (vec3 f32)
local OFF_POS         = 0x200       -- entity world pos vec3 f32
local OFF_SECTION     = 0x29A       -- u16 monster section index (THE gate, cond A)
local OFF_FLAGS638    = 0x638       -- u32; bit 0x8000 = gate cond B
local OFF_DRAWFLAG    = 0x004       -- u32; bit 0x4 = engine "skip draw" (output)
--------------------------------------------------------------------------------

local MON_GIADROME = mhfu.MON_GIADROME
local MON_TIGREX   = mhfu.MON_TIGREX

-- decode a u32 bit pattern as an IEEE-754 single (lua_host exposes only int reads)
local function u32_to_float(u)
  if u == 0 then return 0.0 end
  local sign = (u >> 31) == 1 and -1.0 or 1.0
  local exp  = (u >> 23) & 0xFF
  local mant = u & 0x7FFFFF
  if exp == 0   then return sign * mant * (2.0 ^ -149) end
  if exp == 255 then return sign * math.huge end
  return sign * (1.0 + mant * (2.0 ^ -23)) * (2.0 ^ (exp - 127))
end
local function read_f(addr) return u32_to_float(mhfu.read_u32(addr)) end

-- Swap Giadrome -> Tigrex at native coords before the loading-screen model load.
mhfu.on_quest_targets_building(function(quest)
  if quest == 0 then return end
  if mhfu.quest_has(quest, MON_GIADROME)
     and mhfu.quest_replace_monster(quest, MON_GIADROME, MON_TIGREX) then
    mhfu.log("[tigvis] giadrome -> tigrex (native coords)")
  end
end)

mhfu.on_bigmonster_spawn(function(ent, mtype, slot, hp)
  if mtype ~= MON_TIGREX then return end
  mhfu.entity_set_size(ent, TIGREX_SIZE)
  mhfu.log(string.format("[tigvis] tigrex spawn ent=0x%08X hp=%d sec=%d flags638=0x%08X",
    ent, hp, mhfu.read_u16(ent + OFF_SECTION), mhfu.read_u32(ent + OFF_FLAGS638)))
end)

-- Neutralise any stale action-force closure from a previous hot-reload.
mhfu.on_bigmonster_action(function(ctx) return ctx.action_id end, 100)

local prev_fixed = false

function mhfu_tick()
  mhfu.write_u8(0x090B3A6A, 0xFF)   -- paintball the boss on the map

  local list = mhfu.entities_of_type(MON_TIGREX)
  local ent  = list and list[1]
  if not ent or ent == 0 then prev_fixed = false; return end

  -- physical co-location test (XZ distance, player vs monster)
  local px, pz = read_f(PLAYER_POS_ADDR), read_f(PLAYER_POS_ADDR + 8)
  local mx, mz = read_f(ent + OFF_POS),   read_f(ent + OFF_POS + 8)
  local dx, dz = px - mx, pz - mz
  local d2     = dx * dx + dz * dz

  if d2 < COLOC_DIST * COLOC_DIST then
    -- same section: write his TRUE section (= player's) + satisfy gate cond B
    local parea = mhfu.get_area_index()
    local sec   = mhfu.read_u16(ent + OFF_SECTION)
    if sec ~= parea then
      mhfu.write_u16(ent + OFF_SECTION, parea)
      local f = mhfu.read_u32(ent + OFF_FLAGS638)
      if (f & 0x8000) == 0 then mhfu.write_u32(ent + OFF_FLAGS638, f | 0x8000) end
      if not prev_fixed then
        mhfu.log(string.format("[tigvis] co-located (d=%.0f) sec %d -> %d : forcing visible",
          math.sqrt(d2), sec, parea))
        prev_fixed = true
      end
    end
  else
    prev_fixed = false
  end
end

mhfu.log("[tigvis] registered — Giadrome->Tigrex spawn-visibility fix")
