/*
 * monster_nameplates — floating name + HP text over every monster in the
 * current map section, toggled in-game by DOUBLE-TAP SELECT.
 *
 * Mechanism: a POSTFIX wrapper on the HUD master-draw call site
 * (0x0888FDD4 `jal 0x08891B14`) runs our code each HUD frame, after the
 * engine has drawn its 6 banner layers, with the HUD context ($a0) in hand
 * and the GE 2D dlist still open. For each registry monster we project its
 * world position to screen using the engine's own live camera matrices
 * (VIEW 0x099998B0 + PROJ 0x09999930 — see docs/agent_hud_banners.md) and draw
 * a label by reusing the engine's banner TEXT-DRAW primitive 0x0889377C(ctx,
 * desc). (A hand-built lookAt had the wrong handedness — use the engine mats.)
 *
 * v0.1 = text label "<Name> <HP>". Graphical bar = next iteration.
 */
#include "mhfu/mhfu.h"
#include <pspctrl.h>

#define MOD_ID         "monster_nameplates"

/* --- engine anchors (MHFU EU) --- */
#define HUD_CALL_SITE  0x0888FDD4u   /* jal 0x08891B14 (HUD master draw)      */
#define HUD_TARGET     0x08891B14u
#define TEXT_DRAW      0x0889377Cu   /* f(ctx, TextDesc*) — UTF-16LE          */
#define CAM_TARGET     0x09998D50u   /* vec3 look-at (= player world pos)     */
#define CAM_OFFSET     0x09998E10u   /* vec3 eye-target (eye cell unreliable) */
#define ENT_REG        0x09C1213Cu   /* u32[] monster entity pointers         */
#define REG_SLOTS      32

/* The engine's live camera matrices (column-major). Using them directly =
 * pixel-exact (a hand-built lookAt had the wrong handedness). VIEW maps the
 * look-at target -> origin; PROJ is fovy 50 / aspect 480:272 / near 30. */
#define E_VIEW         0x099998B0u
#define E_PROJ         0x09999930u
#define SCR_CX         240.0f
#define SCR_CY         136.0f      /* PSP viewport y_scale is -136 */

typedef void (*text_draw_fn)(unsigned int ctx, void *desc);

/* 12-byte engine text descriptor (banner template: +4..7 = 0e,0e,00,0e). */
struct TextDesc {
    unsigned short x, y;
    unsigned char  b4, b5, b6, adv;
    unsigned int   strp;            /* -> UTF-16LE, 0x0000 terminated        */
};

static volatile int g_active   = 0;
static int          g_tap_age  = 1000000;
static int          g_prev_sel = 0;

struct V3 { float x, y, z; };
static inline V3 mkv(float a,float b,float c){ V3 r; r.x=a;r.y=b;r.z=c; return r; }

/* live engine matrices, snapshotted once per HUD frame */
static float g_view[16], g_proj[16];
static void load_matrices(void){
    for (int i = 0; i < 16; i++){
        g_view[i] = mhfu_read_f32(E_VIEW + i*4);
        g_proj[i] = mhfu_read_f32(E_PROJ + i*4);
    }
}
/* column-major 4x4 * vec4 */
static inline void mat_vec(const float *m, float x, float y, float z, float w, float *o){
    o[0] = m[0]*x + m[4]*y + m[8] *z + m[12]*w;
    o[1] = m[1]*x + m[5]*y + m[9] *z + m[13]*w;
    o[2] = m[2]*x + m[6]*y + m[10]*z + m[14]*w;
    o[3] = m[3]*x + m[7]*y + m[11]*z + m[15]*w;
}

/* world -> screen pixel via engine view+proj. returns 1 if in front. */
static int project(V3 p, float *sx, float *sy){
    float v[4], c[4];
    mat_vec(g_view, p.x, p.y, p.z, 1.0f, v);
    mat_vec(g_proj, v[0], v[1], v[2], v[3], c);
    float w = c[3];
    if (w < 1.0f) return 0;                  /* behind camera */
    *sx = c[0] / w * SCR_CX + SCR_CX;
    *sy = c[1] / w * (-SCR_CY) + SCR_CY;
    return 1;
}

/* ---------- UTF-16LE string builders ---------- */
static int w_ascii(unsigned short *w, const char *s, int cap){
    int n = 0; while (s && *s && n < cap-1) w[n++] = (unsigned char)*s++; w[n] = 0; return n;
}
static int w_uint(unsigned short *w, int n, unsigned int v, int cap){
    char r[12]; int rn = 0;
    if (v == 0) r[rn++] = '0'; else while (v && rn < 11){ r[rn++] = (char)('0'+(v%10)); v/=10; }
    while (rn && n < cap-1) w[n++] = (unsigned char)r[--rn];
    w[n] = 0; return n;
}

static unsigned short g_wbuf[8][48];
static TextDesc       g_desc;

static void draw_label(unsigned int ctx, int x, int y, unsigned short *w){
    if (x < -48 || x > 528 || y < -16 || y > 288) return;
    g_desc.x = (unsigned short)x; g_desc.y = (unsigned short)y;
    g_desc.b4 = 0x0e; g_desc.b5 = 0x0e; g_desc.b6 = 0x00; g_desc.adv = 0x0e;
    g_desc.strp = (unsigned int)(unsigned long)w;
    ((text_draw_fn)TEXT_DRAW)(ctx, &g_desc);
}

static void poll_toggle(void){
    SceCtrlData pad; sceCtrlPeekBufferPositive(&pad, 1);
    int sel = (pad.Buttons & PSP_CTRL_SELECT) ? 1 : 0;
    if (g_tap_age < 1000000) g_tap_age++;
    if (sel && !g_prev_sel){
        if (g_tap_age < 20){ g_active = !g_active; g_tap_age = 1000000;
                             mhfu_log("[nameplates] %s", g_active ? "ON" : "OFF"); }
        else g_tap_age = 0;
    }
    g_prev_sel = sel;
}

/* per-HUD-frame; a0 = HUD context */
static void hud_postfix(uint32_t ctx){
    poll_toggle();
    if (!g_active) return;
    if (mhfu_get_screen_state() != 17) return;       /* in-area only */
    load_matrices();
    int slot = 0;
    for (int i = 0; i < REG_SLOTS; i++){
        unsigned int ent = mhfu_read_u32(ENT_REG + i*4);
        if (ent < 0x08000000u || ent >= 0x0A000000u) continue;
        unsigned char type = mhfu_entity_type(ent);
        if (type == 0 || type == 0xFF) continue;
        unsigned int  hp = mhfu_entity_hp(ent);
        if (hp == 0 || hp > 60000) continue;
        mhfu_vec3_t mp = mhfu_entity_pos(ent);
        float size = mhfu_entity_size(ent); if (size <= 0.f) size = 1.f;
        V3 p = mkv(mp.x, mp.y + 150.0f + size*120.0f, mp.z);   /* lift to head */
        float sx, sy;
        if (!project(p, &sx, &sy)) continue;
        unsigned short *w = g_wbuf[slot & 7]; slot++;
        const char *nm = mhfu_monster_name(type);
        int n = w_ascii(w, nm ? nm : "Monster", 48);
        if (n < 44){ w[n++] = ' '; w[n] = 0; n = w_uint(w, n, hp, 48); }
        draw_label(ctx, (int)sx - 20, (int)sy, w);
    }
}

static int np_init(void){
    mhfu_hook_rc_t rc = mhfu_install_call_wrapper(HUD_CALL_SITE, HUD_TARGET,
                                                  hud_postfix, MHFU_WRAP_POSTFIX, MOD_ID);
    if (rc != MHFU_HOOK_OK){ mhfu_log("[nameplates] install failed rc=%d", (int)rc); return -1; }
    mhfu_log("[nameplates] installed — double-tap SELECT to toggle");
    return 0;
}

MHFU_MOD(.id = MOD_ID, .version = "0.1", .init = np_init);
