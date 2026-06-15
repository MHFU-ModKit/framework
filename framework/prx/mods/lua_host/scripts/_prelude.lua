-- _prelude.lua — ergonomic OO layer over the flat mhfu.* C bindings.
--
-- Run by lua_host BEFORE any user mod (embedded, not a memstick mod). Builds:
--   * mhfu.mem.*     — raw memory read/write (aliases of the flat bindings)
--   * mhfu.world.*   — global state: player, entities, screen/area, paint_map
--   * Entity handles — mhfu.entity.wrap(ptr) -> object with :pos()/:force_aggro()/…
--   * a Player handle — mhfu.world.player()
--
-- The flat bindings (mhfu.read_u32, mhfu.entity_pos, …) stay available, so
-- existing scripts keep working; this is purely additive sugar.

local M = mhfu

------------------------------------------------------------------- mhfu.mem
M.mem = {
  read_u8   = M.read_u8,   read_u16 = M.read_u16,  read_u32 = M.read_u32,
  write_u8  = M.write_u8,  write_u16= M.write_u16, write_u32= M.write_u32,
  read_f32  = M.read_f32,  write_f32= M.write_f32, valid    = M.mem_valid,
}

-- Accept a handle ({:pos()}), a {x,y,z}/{x=,y=,z=} table, or bare x,y,z.
local function to_xyz(a, b, c)
  if type(a) == "table" then
    if a.pos then return a:pos() end
    if a.x then return a.x, a.y, a.z end
    return a[1], a[2], a[3]
  end
  return a, b, c
end

---------------------------------------------------------------- Entity class
local Entity = {}
Entity.__index = Entity

function M.entity_wrap(ptr) return setmetatable({ ptr = ptr }, Entity) end
M.entity = M.entity or {}
M.entity.wrap = M.entity_wrap

function Entity:valid()      return self.ptr ~= 0 and M.entity_alive(self.ptr) end
function Entity:type()       return M.entity_type(self.ptr) end
function Entity:hp()         return M.entity_hp(self.ptr) end
function Entity:size()       return M.entity_size(self.ptr) end
function Entity:set_size(v)  M.entity_set_size(self.ptr, v); return self end
function Entity:pos()        return M.entity_pos(self.ptr) end
function Entity:set_pos(a,b,c) local x,y,z = to_xyz(a,b,c); M.entity_set_pos(self.ptr,x,y,z); return self end
function Entity:yaw()        return M.entity_yaw(self.ptr) end
function Entity:set_yaw(v)   M.entity_set_yaw(self.ptr, v); return self end
function Entity:ai_state()   return M.entity_ai_state(self.ptr) end
function Entity:set_ai_state(v) M.entity_set_ai_state(self.ptr, v); return self end
function Entity:engaged()    return M.entity_engaged(self.ptr) end
function Entity:set_engaged(b) M.entity_set_engaged(self.ptr, b); return self end
function Entity:section()    return M.entity_section(self.ptr) end
function Entity:set_section(v) M.entity_set_section(self.ptr, v); return self end
function Entity:calm()       M.entity_calm(self.ptr); return self end

-- Clone this monster (same species). Returns a NEW Entity handle (a real,
-- ticking, rendered monster sharing this one's model/overlay) or nil. The
-- species must be resident (clone a Tigrex only where a Tigrex is loaded).
function Entity:clone()
  local p = M.entity_clone(self.ptr)
  if not p or p == 0 then return nil end
  return M.entity_wrap(p)
end

-- Render-fix: make him visible in `section` (default = the player's area).
function Entity:make_visible(section)
  M.entity_make_visible(self.ptr, section or M.get_area_index()); return self
end

-- XZ-plane distance to a target (handle / table / x,z).
function Entity:dist_to(target)
  local tx, _, tz = to_xyz(target)
  local mx, _, mz = self:pos()
  local dx, dz = tx - mx, tz - mz
  return math.sqrt(dx*dx + dz*dz)
end

-- Force this monster to hunt `target` (handle/table). Writes the full engine
-- engage signature (pursuit vec + engage + ai_state + acquired) coherently.
function Entity:force_aggro(target)
  local tx, ty, tz = to_xyz(target)
  M.entity_force_aggro(self.ptr, tx, ty, tz); return self
end

-- Teleport to `offset` units past `target` along +X, then fix visibility.
function Entity:teleport_near(target, offset)
  local tx, ty, tz = to_xyz(target)
  self:set_pos(tx + (offset or 0), ty, tz)
  self:make_visible()
  return self
end

----------------------------------------------------------------- Player class
local Player = {}
Player.__index = Player
local PLAYER = setmetatable({}, Player)

function Player:pos()          return M.player_pos() end
function Player:hp()           return M.get_player_hp() end
function Player:area()         return M.get_area_index() end
function Player:screen_state() return M.get_screen_state() end
function Player:dist_to(ent)
  local tx, _, tz = to_xyz(self)
  local mx, _, mz = to_xyz(ent)
  local dx, dz = tx - mx, tz - mz
  return math.sqrt(dx*dx + dz*dz)
end

------------------------------------------------------------------- mhfu.world
M.world = {
  player        = function() return PLAYER end,
  screen_state  = M.get_screen_state,
  area          = M.get_area_index,
  quest_timer   = M.get_quest_timer,
  in_area       = function() return M.get_screen_state() == 17 end,
  paint_map     = M.paint_map,
}

-- All live entities of a type, as handles.
function M.world.entities(type)
  local out = {}
  for _, p in ipairs(M.entities_of_type(type)) do out[#out+1] = M.entity_wrap(p) end
  return out
end
-- First live entity of a type (or nil).
function M.world.first(type)
  local list = M.entities_of_type(type)
  if list and list[1] and list[1] ~= 0 then return M.entity_wrap(list[1]) end
  return nil
end

------------------------------------------------------------------- enums
-- Named monster ids (alias of the flat mhfu.MON_* constants).
M.MON = {
  ANTEKA = M.MON_ANTEKA, POPO = M.MON_POPO,
  TIGREX = M.MON_TIGREX, GIADROME = M.MON_GIADROME,
}
-- Snowy-mountains area_index values (== entity +0x29A section encoding).
-- area_index alone identifies the map+section (it's a global id, not per-map),
-- so AREA.SNOW_S1 == 99 reliably means "snowy mountains, section 1".
M.AREA = { BASECAMP = 98, SNOW_S1 = 99, SNOW_S6 = 100 }

M.log("[prelude] OO layer ready (mhfu.world / mhfu.entity / mhfu.mem / mhfu.MON / mhfu.AREA)")
