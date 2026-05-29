/* Region detection — selects the active address table. */
#include "mhfu/log.h"
#include "internal.h"

static const mhfu_region_addrs_t *g_addrs = 0;

extern "C" const mhfu_region_addrs_t *mhfu_region(void)
{
    return g_addrs;
}

extern "C" int mhfu_region_detect(void)
{
    /* TODO: read the running game's UMD id (sceKernelGetGameInfo /
     * sceUtilityGetSystemParamString) and pick the matching table.
     * For now: default to EU. */
    g_addrs = &MHFU_REGION_EU;
    mhfu_log("[framework] region detection stubbed; defaulting to EU (%s)",
             g_addrs->game_id);
    return 0;
}
