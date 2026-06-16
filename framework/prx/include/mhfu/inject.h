/*
 * mhfu/inject.h — Phase 4 live in-RAM model/skeleton/animation injection.
 *
 * Pushes a Blender-edited big-monster PAC (skeleton + PMO + animation) into the
 * running game with ZERO on-disk DATA.BIN edits. The edit lives as a loose
 * memstick file; this module reads it and overwrites the species' loaded PAC
 * buffer in the model arena in place.
 *
 *   host (Python `mhfu_model.inject`)            PRX (this module)
 *   ----------------------------------           -----------------------------
 *   Blender edit -> repack() -> bytes            mhfu_inject_register(id, path)
 *     write ms0:/.../inject/file_NNNNN.bin       mhfu_inject_tick()  (2 Hz)
 *                                                  stat -> changed?
 *                                                  read file -> xram scratch
 *                                                  locate live buffer (sig scan)
 *                                                  overwrite in place + dcache wb
 *
 * Two effects from ONE in-place overwrite (see docs/ANIMATION_FORMAT.md):
 *   - ANIMATION is read per-frame from `Hierarchy.motion_table` (+0x12C), which
 *     points INTO this buffer -> value edits update the LIVE monster next frame.
 *   - SKELETON is baked into Joint structs at load, and GEOMETRY is compiled into
 *     a renderable at load -> those apply on the next engine rebuild (e.g. the
 *     player re-enters the monster's section); the buffer already holds our bytes.
 *
 * Locate is by CONTENT SIGNATURE (the loader copies the file verbatim — proven by
 * `prx-runtime-monster-load` build 2), NOT a hardcoded runtime address, so it is
 * stable across runs. A layout-changing edit (keyframe-count delta -> different
 * offset table -> header mismatch) will NOT match; that case needs a real reload.
 */
#ifndef MHFU_INJECT_H
#define MHFU_INJECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register a species PAC fileId -> memstick path to watch & inject.
 * `path` is copied. Returns 0 on success, <0 on table-full / bad args. */
int mhfu_inject_register(uint32_t file_id, const char *path);

/* Per-poll driver (call from a worker thread, e.g. lua_host's 2 Hz worker).
 * Re-stats each registered file; on (size,mtime) change, reads it and overwrites
 * the located live buffer. Cheap no-op when nothing changed. */
void mhfu_inject_tick(void);

/* Force a re-read + overwrite of one registered fileId regardless of mtime.
 * Returns the live buffer address it wrote to, or 0 if not located / failed. */
uint32_t mhfu_inject_now(uint32_t file_id);

/* Diagnostic: locate the live PAC buffer for a registered fileId by signature
 * scan of the model arena. Returns the buffer base, or 0 if not found.
 * Requires the file to have been read at least once (needs the signature). */
uint32_t mhfu_inject_locate(uint32_t file_id);

#ifdef __cplusplus
}
#endif

#endif /* MHFU_INJECT_H */
