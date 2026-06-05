-- freecam.lua — toggleable FREE-FLY camera for MHFU.
--
-- Mechanism (C, lua_host): cold-installed mid-function detours on the engine
-- camera builder (0x08886B68): override the EYE position directly (0x088879AC)
-- and the view look-at target = eye + aim direction (matrix-call paths
-- 0x08887A6C / 0x08887C30). Engine + player are otherwise untouched.
--
-- Toggle:  DOUBLE-TAP SELECT  (per-frame in C — reliable).
-- Controls while ON:
--   Left analog stick : fly forward/back + strafe (ground plane, aim-relative)
--   D-pad             : aim yaw / pitch (free-look, smooth)
--   R / L trigger     : fly up / down (world)
--
-- This script only sets tunables + logs state transitions.

mhfu.cam_config(25.0, 0.035, 300.0)   -- move units/frame, rotate rad/frame, look distance

local was = false
function mhfu_tick()
    local a = mhfu.freecam_active()
    if a ~= was then
        mhfu.log(a and "[freecam] ON (double-tap SELECT)" or "[freecam] OFF")
        was = a
    end
end
