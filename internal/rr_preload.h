/* rr_preload.h - Launch-time in-process load.
 *
 * Starts the title with CREATE_SUSPENDED, stages a two-call stub in its
 * address space, and resumes: the stub sleeps until the loader is free,
 * then LoadLibraryA's the module from inside the process. Cross-process
 * writes only happen before the title has run a single instruction, which
 * is the window EAC's handle protection does not cover.
 */
#ifndef RR_PRELOAD_H
#define RR_PRELOAD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RR_PRELOAD_RESULT {
    uint32_t pid;
    uint32_t thread_id;
} RR_PRELOAD_RESULT;

/* game_exe is the full path to the title's exe; dll_path is absolute or
 * relative to our own working directory. delay_ms is how long the staged
 * stub waits before touching the loader. Return 1 on success. */
int rr_preload_dll(const char* game_exe, const char* dll_path, unsigned delay_ms,
                   RR_PRELOAD_RESULT* out, char* err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* RR_PRELOAD_H */
