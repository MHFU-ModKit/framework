-- model_inject.lua — Phase 4 live model/skeleton/anim injection demo.
--
-- Registers a Blender-edited big-monster PAC to be injected into the running
-- game with ZERO on-disk DATA.BIN edits. The C worker (mhfu_inject_tick, 2 Hz)
-- watches the memstick inject file and overwrites the species' loaded buffer in
-- place: animation value edits update the LIVE monster next frame; skeleton /
-- geometry edits apply on the next engine rebuild (re-enter the monster's
-- section).
--
-- HOST WORKFLOW (no game files touched):
--   1. Edit the monster in Blender (the mhfu addon), or run the example:
--        PYTHONPATH=tools python tools/mhfu_model/examples/edit_anim_demo.py
--   2. Push it to the live game:
--        PYTHONPATH=tools python -m mhfu_model.inject workspace/modified/file_06134.bin
--      (writes ms0:/PSP/PLUGINS/mhfu_framework/inject/file_06134.bin atomically)
--   3. Watch framework.log for "[inject] applied file=6134 -> 0x09xxxxxx".
--
-- Run this script ALONE (it defines no mhfu_tick, but keep it simple): deploy to
-- ms0:/PSP/PLUGINS/mhfu_framework/mods/ alongside the framework.

-- em75 = Tigrex; PAC = file_06134 (em_id 0x4B + 0x17AB index 6134). Swap the id
-- + path for a different species.
local TIGREX_PAC_ID = 6134
local INJECT_PATH   = "ms0:/PSP/PLUGINS/mhfu_framework/inject/file_06134.bin"

if mhfu.inject_register(TIGREX_PAC_ID, INJECT_PATH) then
    mhfu.log("[model_inject] watching " .. INJECT_PATH ..
             " for file " .. TIGREX_PAC_ID)
else
    mhfu.log("[model_inject] inject_register FAILED")
end
