/*
 * Mod descriptor + registration.
 *
 * Every mod — whether statically linked into mhfu_framework.prx or (later)
 * loaded as a separate PRX — describes itself with one POD descriptor and
 * registers it via MHFU_MOD(). The framework collects all descriptors,
 * resolves requires/conflicts, then calls each init() in dependency order.
 *
 * The descriptor is plain data placed in a linker section (no global
 * constructor runs at load — safe on PSP, where C++ static-init order is
 * unspecified). Keep init()/shutdown() free of work that assumes another
 * mod has already run; declare ordering via `requires` instead.
 *
 * Example:
 *   static int  mymod_init(void)      { ... return 0; }
 *   static void mymod_shutdown(void)  { ... }
 *   MHFU_MOD(.id = "my_mod", .version = "1.0",
 *            .conflicts = "popo_heading_hook",
 *            .init = mymod_init, .shutdown = mymod_shutdown);
 *
 * `needs` / `conflicts` are space-separated mod-id lists (or NULL).
 */
#ifndef MHFU_MOD_H
#define MHFU_MOD_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;
    const char *version;
    const char *needs;       /* space-separated mod ids, or NULL */
    const char *conflicts;   /* space-separated mod ids, or NULL */
    int  (*init)(void);      /* return 0 = ok, negative = refuse to load */
    void (*shutdown)(void);  /* may be NULL */
} mhfu_mod_t;

/* Emit a descriptor into the 'mhfu_mods' section. GNU ld auto-defines
 * __start_mhfu_mods / __stop_mhfu_mods so the framework can iterate the
 * table. `used` keeps it past --gc-sections; internal linkage avoids any
 * cross-TU name clash. */
#define MHFU_MOD(...) \
    __attribute__((used, section("mhfu_mods"), aligned(4))) \
    static const mhfu_mod_t mhfu__mod_descriptor = { __VA_ARGS__ }

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_MOD_H */
