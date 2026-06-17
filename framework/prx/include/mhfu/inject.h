/*
 * mhfu/inject.h — Phase 4 live in-RAM model/skeleton/animation injection
 * (FUComplete-spirit raw-file redirect, RE'd 2026-06-17).
 *
 * Pushes a Blender-edited big-monster PAC (skeleton + PMO + animation) into the
 * running game with ZERO on-disk DATA.BIN edits. The edit lives as a loose
 * memstick file; this module overwrites the species' RAW pre-transform PAC buffer
 * so the engine runs its normal restructure on OUR data.
 *
 *   host (Python `mhfu_model.inject`)            PRX (this module)
 *   ----------------------------------           -----------------------------
 *   Blender edit -> repack() -> bytes            mhfu_inject_register(id, path)
 *     write ms0:/.../inject/file_NNNNN.bin       + file_NNNNN.bin.orig (original)
 *                                                mhfu_inject_tick()  (2 Hz worker)
 *                                                  stat -> changed? -> read to xram
 *                                                  scan_descriptor_overwrite():
 *                                                    [0x09A4F0D0] table; per slot
 *                                                    buffer@+4 == original? -> memcpy
 *                                                    edited bytes + dcache wb
 *
 * THE SEAM (memory `phase4-descriptor-table-seam`): the overlay descriptor table
 * [0x09A4F0D0] (stride 0xC: fileId@+2, buffer@+4) exposes each loaded resource's
 * RAW on-disk PAC buffer (count=7 big-mon model: skeleton/pmo/TMH/anim), byte-
 * identical to data_files/file_0NNNNN.bin, BEFORE the overlay restructures it. We
 * overwrite that buffer with our edited PAC -> the engine restructures our data.
 *
 * Content-gated: we write only when buffer@+4 byte-matches the ORIGINAL file
 * (a sibling <path>.orig), so a same-size look-alike never matches and the write
 * is idempotent. The overwrite must land BEFORE the one-shot transform reads the
 * buffer; the 2 Hz tick covers the multi-second section-load window, and
 * mhfu_inject_burst() hammers it at ~200 Hz for a guaranteed pre-transform hit.
 *
 * IMPORTANT (RE'd live 2026-06-17): a quest's big monster does NOT necessarily use
 * the file_0{em_id+0x17AB} PAC — link the on-screen entity to its model buffer to
 * find the real file (e.g. the native-Tigrex-quest Tigrex uses file_06185, NOT
 * file_06134). Engine fileId = our file index + 1 (off-by-one).
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

/* Tight ~200 Hz descriptor-scan burst (~1.5 s) to overwrite the raw model buffer
 * the instant it is populated, before the overlay transform reads it. Call from a
 * load/section-entry event for a guaranteed pre-transform hit. Blocks the caller. */
void mhfu_inject_burst(void);

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
