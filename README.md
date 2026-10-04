<p align="center">
  <img src="misc/banner.svg" width="720" alt="MHFU-FRAMEWORK">
</p>

# MHFU ModKit — the framework

> **Archived.** This code now lives in [MHFU-ModKit/modkit](https://github.com/MHFU-ModKit/modkit), at [`framework`](https://github.com/MHFU-ModKit/modkit/tree/main/framework). Open issues and pull requests there.

**A runtime mod framework for Monster Hunter Freedom Unite (PSP)** — load mods into the running
game without patching or redistributing the ISO. Write a mod as a Lua script on the memory stick
and hot-reload it while the game runs, or compile one into the plugin for full hook access.

> **Status: working, but the mod API is still moving.** The framework loads and runs on both
> PPSSPP and real PSP hardware, and the features below are proven in-game. The authoring surface
> is not yet stable — expect breaking changes between commits.

## What works today

| | Feature | Notes |
|---|---|---|
| ✅ | **One plugin, many mods** | Mods compose statically into `mhfu_framework.prx`; `mods.manifest` is the load list |
| ✅ | **Lua mods, hot-reloaded** | Edit a `.lua` on the memory stick, changes apply live — no rebuild, no reboot |
| ✅ | **C++ descriptor mods** | `MHFU_MOD(...)` with `init` / `shutdown`, `needs` / `conflicts` |
| ✅ | **Hook arbitration** | Priority chains so two mods sharing a hook resolve instead of clobbering |
| ✅ | **AI override events** | Force a big monster's action coherently; observe spawn / death / damage / per-frame AI |
| ✅ | **Big-monster AI takeover** | One word into the species vtable wraps the per-frame AI step or the enter-action — no overlay to author, compile or inject |
| ✅ | **Quest monster injection** | Add or replace a quest's monsters at load; added monsters fight properly |
| ✅ | **Live model injection** | Replace a monster's model, skeleton, textures and animation in RAM, zero disk edits |
| ✅ | **Ported monsters** | A monster ported from MHP3rd runs on its own rig and plays its own clips at their authored length; its behaviour is scripted in Lua against a host species (`docs/MOD_PORTED_MONSTER.md`) |
| ✅ | **Real PSP hardware** | PRO CFW, via a kernel bootstrap; includes a volatile-memory interposer for extra RAM |
| ✅ | **Free camera / HUD overlays** | Toggleable freecam, projected monster nameplates |
| 🚧 | **Monster visual effects** | A mod can fire the engine's own effects at any bone; what each id looks like is not yet catalogued |
| 🚧 | **Mod API stability** | Surface still changing; no versioning guarantees yet |
| 🚧 | **Multiple damaging big monsters** | Hard ceiling of **2** — architectural (`docs/AI_SCRIPTING_ENGINE.md`) |
| 🚧 | **Packaging / distribution** | No release builds yet; build from source |

**A note on ported monsters:** every pre-World Monster Hunter game ships monster AI as compiled
native code, so **no AI is portable from any of them**. A ported monster's model, skeleton,
textures and animations transfer; its *behaviour* is scripted against an existing MHFU species.
That is a property of the games, not a gap in this tooling.

## Quickstart — your first mod

The fastest path is a Lua script. With the framework installed (below), drop this in
`<memstick>/PSP/PLUGINS/mhfu_framework/mods/hello.lua`:

```lua
-- hello.lua — shout when a big monster spawns, and shrink it
mhfu.on_bigmonster_spawn(function(ent, mtype, slot, hp)
    mhfu.log("[hello] monster type=%d hp=%d spawned at slot %d", mtype, hp, slot)
    mhfu.entity_set_size(ent, 0.5)      -- half-size monster
end)

mhfu.on_bigmonster_damaged(function(ent, mtype, amount, hp, slot)
    mhfu.log("[hello] took %d damage, %d hp left", amount, hp)
end)
```

Start a quest. Edit the file, save, and the change applies without restarting the game.

The Lua surface covers entities (`entity_hp`, `entity_pos`, `entity_set_size`, `entity_clone`,
`entity_force_aggro`, …), quests (`quest_add_monster`, `quest_replace_monster`), the camera,
model injection (`inject_register`, `inject_relocate`), the AI override chain
(`on_bigmonster_action` and friends) and the native seams (`em_request`, `em_substitute`,
`em_rule`). A per-tick `mhfu_tick()` is called if you define it. The full reference is
[`framework/prx/mods/lua_host/README.md`](framework/prx/mods/lua_host/README.md); worked mods
are in the [`example-mods`](https://github.com/MHFU-ModKit/example-mods) repo.

## Install

The build produces a single `mhfu_framework.prx`. Place it as:

```
<memstick>/PSP/PLUGINS/mhfu_framework/
    mhfu_framework.prx
    plugin.ini
    mods/*.lua           your Lua mods
    framework.log        written at runtime
```

`plugin.ini` — it needs real sections; a flat `key=value` file is **silently ignored**:

```ini
[games]
ULES01213 = true
ULUS10391 = true
ULJM05500 = true
[options]
type = prx
filename = mhfu_framework.prx
name = MHFU Framework
version = 1
```

**⚠️ Plugins load on cold boot only.** Launching from a savestate runs the game *without* the
framework — which looks exactly like a mod that does nothing.

PPSSPP memstick paths: macOS `~/Documents/PPSSPP/PSP/`, Linux `~/.config/ppsspp/PSP/`,
Windows `%USERPROFILE%/Documents/PSP/`. Real hardware needs PRO CFW and has extra requirements —
[`docs/REALHW_PLUGIN_LOAD.md`](docs/REALHW_PLUGIN_LOAD.md).

## Build from source

```bash
git clone https://github.com/MHFU-ModKit/framework.git
cd framework/framework/prx
make          # → mhfu_framework.prx, composing the mods in build/mods.manifest
```

Requires **Docker** (the pspdev toolchain ships as `pspdev/pspdev:latest`) and Python 3 on the
host for the two build helpers in `tools/`. Which mods are baked in is `build/mods.manifest` —
one per line, `#` disables. Authoring, the SDK, arbitration and the hard-won gotchas are in
[`framework/prx/README.md`](framework/prx/README.md).

## Layout

```
framework/prx/         SDK headers, C++ core, mods, build manifest — the product
  mods/lua_host/       the Lua VM, its mhfu.* API, the embedded prelude and port library
  mods/em_vhook/       the native AI seams (substitution, requests, distance rules)
tools/                 the four helpers the build and its tests need
docs/                  architecture, the overlay ABI, the AI engine, real hardware, ported monsters
misc/                  the banner
```

## Documentation

[`docs/FRAMEWORK_ARCHITECTURE.md`](docs/FRAMEWORK_ARCHITECTURE.md) is the map;
[`docs/AI_SCRIPTING_ENGINE.md`](docs/AI_SCRIPTING_ENGINE.md) and
[`docs/EM_OVERLAY_ABI.md`](docs/EM_OVERLAY_ABI.md) are how the monsters work;
[`docs/MOD_PORTED_MONSTER.md`](docs/MOD_PORTED_MONSTER.md) is how to script a ported one;
[`docs/REALHW_PLUGIN_LOAD.md`](docs/REALHW_PLUGIN_LOAD.md) is the real PSP. These documents were
written alongside the reverse engineering and refer to tooling and notes that live in the
upstream research repository; where a path does not exist here, that is why.

## No game data is included

This repository contains **no game files, no extracted assets, no artwork** — not the ISO, not
decrypted archives, not models or textures, not dumped tables. All of it is gitignored and is
reproduced from your own legally obtained copy of the game (see the `formats` repo's
`docs/ASSETS.md`). Everything targets **MHFU EU (ULES01213)**; addresses will not line up with a
JP or NA build.

## About this repository

`framework` is one of the [MHFU-ModKit](https://github.com/MHFU-ModKit) repositories. They are cut
from one upstream research repository and re-published from it, so they move in lockstep — a
file that appears in two of them is the same file at the same commit. Pull requests are welcome
here; an accepted one is applied upstream and comes back in the next export, which is why
`main` only takes changes through PRs. Issues are welcome for bugs, questions and findings alike.

The siblings:

- [`example-mods`](https://github.com/MHFU-ModKit/example-mods) — Lua mods and port manifests to learn from and drop on a memory stick
- [`monster-editor`](https://github.com/MHFU-ModKit/monster-editor) — the desktop editor for ported monsters
- [`hud`](https://github.com/MHFU-ModKit/hud) — a live read-only HUD and AI editor over the PPSSPP debugger
- [`blender-addon`](https://github.com/MHFU-ModKit/blender-addon) — import, edit and export big monsters in Blender
- [`formats`](https://github.com/MHFU-ModKit/formats) — the file-format library, ISO extraction and the MHP3rd→MHFU porter

## License

[MIT](LICENSE). Not affiliated with or endorsed by Capcom. Monster Hunter is a trademark of
Capcom Co., Ltd.

## Credits

Built on the work of the MHFU/MHP2G reverse-engineering community:
[mhff](https://github.com/svanheulen/mhff) and [mhef](https://github.com/svanheulen/mhef)
(file formats and encryption), [mhfu-re](https://github.com/SiD3W4y/mhfu-re),
[tclamb/mhp2g-decomp](https://github.com/tclamb/mhp2g-decomp),
[FUComplete](https://github.com/FUComplete/Patch), and
[PPSSPP](https://github.com/hrydgard/ppsspp), without whose debugger none of this would exist.
