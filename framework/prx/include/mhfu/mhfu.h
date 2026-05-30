/*
 * MHFU modding-framework SDK — umbrella include.
 *
 * #include "mhfu/mhfu.h" to pull the whole public API. Build with the
 * pspdev toolchain (docker pull pspdev/pspdev). See framework/prx/README.md
 * and the mods under framework/prx/mods/ for working examples.
 *
 * Region note: anchor PCs / cell addresses are MHFU EU (ULES01213) for
 * now. The region table (addresses.h) is the single source of truth;
 * mods read addresses through the memory/entity helpers, never literals.
 */
#ifndef MHFU_MHFU_H
#define MHFU_MHFU_H

#include "events.h"
#include "ids.h"
#include "memory.h"
#include "entity.h"
#include "monster.h"
#include "quest.h"
#include "hooks.h"
#include "ai.h"
#include "ai_actions.h"
#include "mod.h"
#include "log.h"
#include "addresses.h"

#endif /* MHFU_MHFU_H */
