/*
 * Named game constants. Verified emIds (monster type byte at
 * entity+0x1E8 / quest record emId). The list is INCOMPLETE — add ids as
 * they're confirmed; unknown monsters keep their raw 0xNN.
 */
#ifndef MHFU_IDS_H
#define MHFU_IDS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MON_ANTEKA   = 0x45,
    MON_POPO     = 0x46,
    MON_TIGREX   = 0x4B,
    MON_GIADROME = 0x4D,
} mhfu_monster_id_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_IDS_H */
