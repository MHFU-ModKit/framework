# Lua embedding spike (Phase 0)

Throwaway de-risking spike for the Lua modding platform (see
`docs/LUA_MODDING_PLATFORM_PLAN.md` → "Phase 0 RESULTS"). **PASS** — kept for
reproducibility, not part of the shipped framework.

- `main.c` + `Makefile` — runnable **EBOOT** that creates a sandboxed Lua 5.4
  VM, runs a representative mod-ish script, and logs live heap demand to
  `ms0:/lua_spike.log`. Run on PPSSPP; this is what proved Lua runs on Allegrex
  and costs ~8 KB steady / ~12 KB peak per VM.
- `prx_link_test.cpp` + `Makefile.prx` — **PRX** link test under the framework's
  exact light-C++ flags (`-fno-exceptions -fno-rtti -fno-threadsafe-statics`,
  **no `-lstdc++`**). Proves Lua needs no C++ runtime and links into a PRX.

## Build

```bash
docker run --rm -v "$(pwd)":/work -w /work pspdev/pspdev:latest make
docker run --rm -v "$(pwd)":/work -w /work pspdev/pspdev:latest make -f Makefile.prx
```

## Key facts established

- pspdev image ships `lua54` 5.4.6 prebuilt, `luaconf.h` already
  `#define LUA_32BITS 1` → `sizeof(lua_Number)==sizeof(lua_Integer)==4`.
  Just `#include "lua.h"` and link `-llua -lm`.
- Sandbox (open only base/table/string/math) dead-strips io/os/debug/package/
  coroutine from the binary — verified with `psp-nm`.
