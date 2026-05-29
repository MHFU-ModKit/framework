/*
 * Framework logging. Lines go to
 * ms0:/PSP/PLUGINS/mhfu_framework/framework.log (PPSSPP: configured
 * memstick dir; real PSP: the stick).
 */
#ifndef MHFU_LOG_H
#define MHFU_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void mhfu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_LOG_H */
