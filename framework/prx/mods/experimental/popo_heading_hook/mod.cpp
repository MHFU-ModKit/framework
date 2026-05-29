/*
 * popo_heading_hook (EXPERIMENTAL) — postfix wrapper on the per-entity
 * transform builder (0x088652DC) via its popo-path OVL_A caller
 * (0x09AC51AC). On each popo transform tick, an engage-gated helper steps
 * the popo toward the next popo in slot order + sets its yaw, producing a
 * smooth bounded chase for popos the player has engaged.
 *
 * STATUS: works only when the caller patch lands before PPSSPP's JIT
 * pre-caches the OVL_A block (patched on section-1 entry, before any
 * popo state transition). Idle popos left fully native; engaged popos
 * (+0x05DC>=1.0) are scripted. crash-fixed with a hard section-box clamp
 * (Section 28e). Reference port; off by default.
 */
#include <pspthreadman.h>
#include "mhfu/mhfu.h"
#include "mhfu/mips.h"

#define MOD_ID        "popo_heading_hook"
#define HDG_ROTATOR   0x088652DCu
#define OVL_CALLER    0x09AC51ACu   /* `jal 0x088652DC` in popo's tick path */
#define POPO_TYPE     0x46
#define TARGET_AREA   99
#define STUB_INSNS    16
#define OFF_ENGAGE    0x05DC
#define OFF_YAW       0x1F4

/* snow-section-1 roam box (clamp; off-map writes crash the zone code) */
#define BOX_X_MIN 14000.0f
#define BOX_X_MAX 18000.0f
#define BOX_Z_MIN  9000.0f
#define BOX_Z_MAX 12000.0f
#define CHASE_GAP   160.0f
#define CHASE_SPEED   4.0f

static volatile uint32_t g_stub[STUB_INSNS];
static volatile uint32_t g_enable    = 1;
static volatile uint32_t g_hit       = 0;
static volatile int      g_installed = 0;

extern "C" float atan2f(float, float);

static int in_box(float x, float z)
{
    return x >= BOX_X_MIN && x <= BOX_X_MAX && z >= BOX_Z_MIN && z <= BOX_Z_MAX;
}

/* Sorted-by-slot popo list from the registry. */
static int popo_list(uint32_t *out)
{
    int n = 0;
    for (int s = 1; s < MHFU_ENTITY_REGISTRY_SLOTS && n < MHFU_ENTITY_REGISTRY_SLOTS; s++) {
        uint32_t p = mhfu_entity_at(s);
        if (p && mhfu_entity_type(p) == POPO_TYPE) out[n++] = p;  /* registry is slot-ordered */
    }
    return n;
}

/* Called from the stub with $a0 = entity, BEFORE the real transform body. */
extern "C" void mhfu_hdg_chase_helper(uint32_t entity)
{
    g_hit++;
    if (!g_enable) return;
    if (mhfu_get_screen_state() != 17) return;
    if (mhfu_entity_type(entity) != POPO_TYPE) return;

    uint32_t list[MHFU_ENTITY_REGISTRY_SLOTS];
    int n = popo_list(list);
    if (n < 2) return;
    int pos = -1;
    for (int i = 0; i < n; i++) if (list[i] == entity) { pos = i; break; }
    if (pos < 0) return;
    uint32_t target = list[(pos + 1) % n];

    /* engage gate — idle popos stay 100% engine-native */
    if (mhfu_read_f32(entity + OFF_ENGAGE) < 0.5f) return;

    mhfu_vec3_t s = mhfu_entity_pos(entity), t = mhfu_entity_pos(target);
    if (!in_box(t.x, t.z) || !in_box(s.x, s.z)) return;
    float dx = t.x - s.x, dz = t.z - s.z;
    float magsq = dx*dx + dz*dz;
    if (magsq < 1.0f) return;
    float inv = 1.0f / __builtin_sqrtf(magsq);
    float ux = dx * inv, uz = dz * inv;
    if (__builtin_sqrtf(magsq) <= CHASE_GAP) return;   /* in gap → idle native */

    mhfu_vec3_t np = s;
    np.x = s.x + ux * CHASE_SPEED;
    np.z = s.z + uz * CHASE_SPEED;
    if (np.x < BOX_X_MIN) np.x = BOX_X_MIN;
    if (np.x > BOX_X_MAX) np.x = BOX_X_MAX;
    if (np.z < BOX_Z_MIN) np.z = BOX_Z_MIN;
    if (np.z > BOX_Z_MAX) np.z = BOX_Z_MAX;
    mhfu_entity_set_pos(entity, np);   /* also syncs the transform translation row */

    float yaw_rad = atan2f(uz, ux);
    int32_t yaw_hw = (int32_t)(-yaw_rad * (32768.0f / 3.14159265f)) & 0xFFFF;
    mhfu_write_u16(entity + OFF_YAW, (uint16_t)yaw_hw);
}

static void build_stub(void)
{
    uint32_t *s = (uint32_t *)g_stub;
    int i = 0;
    uint32_t helper = (uint32_t)(uintptr_t)&mhfu_hdg_chase_helper;
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
    s[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
    s[i++] = mips_jal(helper);
    s[i++] = MIPS_NOP;
    s[i++] = mips_lw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
    s[i++] = mips_jal(HDG_ROTATOR);
    s[i++] = MIPS_NOP;
    s[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);
    s[i++] = mips_jr(MIPS_REG_RA);
    s[i++] = MIPS_NOP;
    while (i < STUB_INSNS) s[i++] = MIPS_NOP;
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(333 * 1000);
        if (g_installed || mhfu_get_area_index() != TARGET_AREA) continue;
        /* Patch the OVL_A caller the moment section 1 is entered, before
         * any popo state transition JIT-translates the block. */
        if (mhfu_read_u32(OVL_CALLER) != mips_jal(HDG_ROTATOR)) continue;
        if (mhfu_patch_word(OVL_CALLER, mips_jal((uint32_t)(uintptr_t)g_stub), MOD_ID)
            == MHFU_HOOK_OK)
            g_installed = 1;
    }
    return 0;
}

static int hdg_init(void)
{
    build_stub();
    mhfu_flush_caches();
    SceUID th = sceKernelCreateThread(MOD_ID, (SceKernelThreadEntry)worker,
                                      0x18, 0x1000, 0, NULL);
    if (th < 0) return -1;
    sceKernelStartThread(th, 0, NULL);
    return 0;
}

static void hdg_shutdown(void) { g_enable = 0; }  /* caller word restored by framework */

MHFU_MOD(.id = MOD_ID, .version = "0.2-experimental",
         .needs = 0, .conflicts = "popo_vt8_override popo_vt5_freeze tigrex_inject",
         .init = hdg_init, .shutdown = hdg_shutdown);
