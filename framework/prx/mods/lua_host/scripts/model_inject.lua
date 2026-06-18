-- model_inject.lua — Phase 4 live model/skeleton/anim injection demo.
--
-- Registers a Blender-edited big-monster PAC to be injected into the running
-- game with ZERO on-disk DATA.BIN edits. The C worker (mhfu_inject_tick, 2 Hz)
-- watches the memstick inject file, reads it + its `.orig` sibling, and scans the
-- overlay descriptor table [0x09A4F0D0]: when a loaded resource's RAW buffer
-- byte-matches the original, it overwrites it with our edited PAC BEFORE the
-- overlay restructures the data -> the engine rebuilds the monster from OUR bytes.
--
-- HOST WORKFLOW (no game files touched):
--   1. Edit the monster in Blender (the mhfu addon), or run an example editor.
--   2. Push it to the live game (writes the inject file + a .orig copy atomically):
--        PYTHONPATH=tools python -m mhfu_model.inject workspace/extracted/data_files/file_06185.bin
--   3. Cold-boot, enter the Tigrex's section; watch framework.log for
--        "[inject] OVERWROTE raw buffer file=6185 @0x09xxxxxx".
--
-- Deploy to ms0:/PSP/PLUGINS/mhfu_framework/mods/ alongside the framework.

-- IMPORTANT (RE'd live 2026-06-17): the native-Tigrex-quest Tigrex's model is
-- file_06185 (NOT file_06134 / em75 — that file is never loaded for this quest).
-- Confirmed by linking the on-screen Tigrex entity (species 0x4B, size 0.9) to its
-- model buffer. The C side content-matches against file_06185.bin.orig, so the
-- registered id below is only for logging.
local TIGREX_PAC_ID = 6185
local INJECT_PATH   = "ms0:/PSP/PLUGINS/mhfu_framework/inject/file_06185.bin"

if mhfu.inject_register(TIGREX_PAC_ID, INJECT_PATH) then
    mhfu.log("[model_inject] watching " .. INJECT_PATH ..
             " for file " .. TIGREX_PAC_ID)
else
    mhfu.log("[model_inject] inject_register FAILED")
end
