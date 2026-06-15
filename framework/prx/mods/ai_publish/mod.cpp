/*
 * ai_publish — exposes per-big-monster AI decisions to external readers
 * (the live HUD, telemetry tools, etc.) by snooping the action_decided
 * override chain and the spawn/death observe events, then writing a
 * compact table into a fixed PSP RAM scratch region the HUD polls via
 * the PPSSPP debugger socket.
 *
 * No behaviour change — both handlers abstain (return engine_value
 * unchanged). The action_decided hook gives us the engine's pick the
 * instant the engine ticks; while the game is paused no new decisions
 * fire, so the published table naturally freezes at the last-seen value
 * — that's how the HUD keeps showing the action through a pause.
 *
 * Publish layout (little-endian, addr = AI_PUBLISH_BASE):
 *   +0x00  u32  magic     = 'AIPB' = 0x42504941 ('A','I','P','B' little-endian)
 *   +0x04  u32  version   = 1
 *   +0x08  u32  slot_cnt  = 21
 *   +0x0C  u32  stride    = 0x14 (20 bytes/entry)
 *   +0x10  u32  serial    monotonic — incremented on every entry write
 *   +0x14  u32  reserved
 *   +0x18  entry[0]
 *      [0x00] u32  entity_ptr   (0 = empty)
 *      [0x04] u8   monster_type (entity+0x1E8)
 *      [0x05] u8   slot_index   (entity-registry slot 0..20)
 *      [0x06] u16  vt8_input    (last action_decided ctx->vt8_input)
 *      [0x08] u32  engine_value (last action_decided engine_action_id)
 *      [0x0C] u32  decided_serial (global serial at last update)
 *      [0x10] u32  spawn_serial   (global serial at spawn)
 *   …
 *   total: 0x18 + 21*0x14 = 0x18 + 0x19C = 0x1B4 bytes (< 0x200).
 *
 * AI_PUBLISH_BASE = 0x08AE2000. Sits 0x2000 above the documented code
 * cave at 0x08AE0000 (used by mhfu_bot for MIPS stubs); not in any
 * known engine struct. RAM is writable from PRX context.
 *
 * Mutually exclusive with the popo experimental mods (they all claim
 * popo vt[8]) — same conflict rule as ai_demo.
 */
#include "mhfu/mhfu.h"

#define MOD_ID         "ai_publish"
#define PUB_BASE       0x08AE2000u
#define MAGIC          0x42504941u  /* 'AIPB' */
#define VERSION        1u
#define SLOT_COUNT     21
#define ENTRY_STRIDE   0x14u
#define ENTRY_BASE     (PUB_BASE + 0x18u)
#define HDR_SERIAL     (PUB_BASE + 0x10u)

static volatile uint32_t g_serial = 0;

static inline uint32_t entry_addr(int i)
{
    return ENTRY_BASE + (uint32_t)i * ENTRY_STRIDE;
}

static int find_slot_by_ptr(uint32_t ent)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (mhfu_read_u32(entry_addr(i)) == ent) return i;
    }
    return -1;
}

static int find_free_slot(void)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (mhfu_read_u32(entry_addr(i)) == 0u) return i;
    }
    return -1;
}

static void clear_entry(int i)
{
    uint32_t a = entry_addr(i);
    mhfu_write_u32(a + 0x00, 0);
    mhfu_write_u32(a + 0x04, 0);   /* type + slot_idx + vt8_input */
    mhfu_write_u32(a + 0x08, 0);   /* engine_value */
    mhfu_write_u32(a + 0x0C, 0);   /* decided_serial */
    mhfu_write_u32(a + 0x10, 0);   /* spawn_serial */
}

static void write_header(void)
{
    mhfu_write_u32(PUB_BASE + 0x00, MAGIC);
    mhfu_write_u32(PUB_BASE + 0x04, VERSION);
    mhfu_write_u32(PUB_BASE + 0x08, (uint32_t)SLOT_COUNT);
    mhfu_write_u32(PUB_BASE + 0x0C, ENTRY_STRIDE);
    mhfu_write_u32(HDR_SERIAL,      0u);
    mhfu_write_u32(PUB_BASE + 0x14, 0u);
}

/* --- event handlers -------------------------------------------------- */

static uint32_t on_action(const mhfu_action_decision_ctx_t *ctx,
                          uint32_t engine_action_id)
{
    int i = find_slot_by_ptr(ctx->entity_ptr);
    if (i < 0) {
        /* spawn missed (race / cold install) — claim a free slot. */
        i = find_free_slot();
        if (i < 0) return engine_action_id;
        uint32_t a = entry_addr(i);
        mhfu_write_u32(a + 0x00, ctx->entity_ptr);
        /* slot_index unknown — pack 0xFF */
        uint32_t packed = ((uint32_t)ctx->monster_type) |
                          (0xFFu << 8) |
                          ((uint32_t)ctx->vt8_input << 16);
        mhfu_write_u32(a + 0x04, packed);
    }
    uint32_t a = entry_addr(i);
    /* refresh packed word — keep slot_idx as-is by reading first */
    uint32_t cur = mhfu_read_u32(a + 0x04);
    uint8_t slot_idx = (uint8_t)((cur >> 8) & 0xFF);
    uint32_t packed = ((uint32_t)ctx->monster_type) |
                      ((uint32_t)slot_idx << 8) |
                      ((uint32_t)ctx->vt8_input << 16);
    mhfu_write_u32(a + 0x04, packed);
    mhfu_write_u32(a + 0x08, engine_action_id);
    uint32_t s = ++g_serial;
    mhfu_write_u32(a + 0x0C, s);
    mhfu_write_u32(HDR_SERIAL, s);
    return engine_action_id;   /* abstain */
}

static void on_spawn(const mhfu_bigmonster_spawn_ctx_t *ctx)
{
    int i = find_slot_by_ptr(ctx->entity_ptr);
    if (i < 0) i = find_free_slot();
    if (i < 0) {
        mhfu_log("[ai_publish] no free slot for spawn ent=0x%08lx",
                 (unsigned long)ctx->entity_ptr);
        return;
    }
    uint32_t a = entry_addr(i);
    mhfu_write_u32(a + 0x00, ctx->entity_ptr);
    uint32_t packed = ((uint32_t)ctx->monster_type) |
                      ((uint32_t)(ctx->slot & 0xFF) << 8) |
                      0u;          /* vt8_input unknown until first decide */
    mhfu_write_u32(a + 0x04, packed);
    mhfu_write_u32(a + 0x08, 0u);  /* engine_value */
    mhfu_write_u32(a + 0x0C, 0u);  /* decided_serial */
    uint32_t s = ++g_serial;
    mhfu_write_u32(a + 0x10, s);   /* spawn_serial */
    mhfu_write_u32(HDR_SERIAL, s);
}

static void on_death(const mhfu_bigmonster_death_ctx_t *ctx)
{
    int i = find_slot_by_ptr(ctx->entity_ptr);
    if (i < 0) return;
    clear_entry(i);
    uint32_t s = ++g_serial;
    mhfu_write_u32(HDR_SERIAL, s);
}

/* --- lifecycle ------------------------------------------------------- */

static int ai_publish_init(void)
{
    write_header();
    for (int i = 0; i < SLOT_COUNT; i++) clear_entry(i);
    mhfu_on_bigmonster_action_decided(on_action, /* priority */ -1000);
    mhfu_on_bigmonster_spawn         (on_spawn,  0);
    mhfu_on_bigmonster_death         (on_death,  0);
    mhfu_log("[ai_publish] registered at 0x%08x (%d slots, stride 0x%x)",
             (unsigned)PUB_BASE, SLOT_COUNT, (unsigned)ENTRY_STRIDE);
    return 0;
}

static void ai_publish_shutdown(void)
{
    mhfu_off_bigmonster_action_decided(on_action);
    mhfu_off_bigmonster_spawn         (on_spawn);
    mhfu_off_bigmonster_death         (on_death);
    /* Wipe the header so a stale magic isn't picked up after PRX
     * unload (e.g. mod hot-reload in a future build). */
    mhfu_write_u32(PUB_BASE + 0x00, 0u);
}

MHFU_MOD(.id = MOD_ID, .version = "1.0",
         .needs = 0,
         .conflicts = "ai_demo popo_vt8_override popo_aggression popo_growth",
         .init = ai_publish_init, .shutdown = ai_publish_shutdown);
