/* rr_map.h - In-process module mapping without the Windows loader.
 *
 * Maps a PE64 DLL into a live target with no LoadLibraryA and no
 * CreateRemoteThread: sections are written with WriteProcessMemory, imports
 * are bound against the target's own loader entries, TLS callbacks and the
 * exception directory are registered, and the entry point runs on a
 * suspended thread that is restored afterwards. The image never enters the
 * target's PEB module list.
 */
#ifndef RR_MAP_H
#define RR_MAP_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RR_MAP_RESULT {
    uint64_t base;      /* remote image base */
    uint32_t size;      /* SizeOfImage */
    DWORD    thread_id; /* thread used for entry-point dispatch */
} RR_MAP_RESULT;

/* Returns 1 on success, 0 on failure with a message in err. A success means
 * the image is mapped and its entry point returned without raising, not that
 * the mapped module finished initializing its own threads.
 */
int rr_map_dll(uint32_t pid, const char* dll_path, RR_MAP_RESULT* out,
               char* err, size_t err_cap);

/* Loader lookups through the target's PEB and LDR list. Both need only a
 * handle the kernel still answers reads for, so they work on a protected
 * process whose write rights are stripped. */
uint64_t rr_remote_peb(HANDLE proc);
int rr_remote_module(HANDLE proc, uint64_t peb, const char* name, uint64_t* base);

#ifdef __cplusplus
}
#endif

#endif /* RR_MAP_H */
