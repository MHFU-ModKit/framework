/*
 * noop — does nothing. Exists only to (a) satisfy the mhfu_mods section
 * anchor (__start/__stop_mhfu_mods) so the PRX links with no real mod, and
 * (b) serve as a bisect build that exercises the framework CORE alone
 * (install worker + mhfu_spawn_poll + section trampoline) with zero mod
 * behavior. Used to isolate a section-1-entry crash: core vs lua_host.
 */
#include "mhfu/mhfu.h"

static int noop_init(void) { mhfu_log("[noop] core-only bisect build"); return 0; }

MHFU_MOD(.id = "noop", .version = "1.0",
         .needs = 0, .conflicts = 0,
         .init = noop_init, .shutdown = 0);
