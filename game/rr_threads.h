/* rr_threads.h - worker creation for the scan jobs.
 *
 * The external client can spawn plain Win32 threads. That is not true inside
 * ROB RUN: EAC reads each thread's Win32StartAddress and terminates the
 * process when it does not resolve into a loaded module, so the internal
 * build routes the same jobs through the process thread pool, whose workers
 * belong to ntdll. The returned handle is a manual-reset completion event so
 * WaitForSingleObject / WaitForMultipleObjects / CloseHandle keep working.
 */
#pragma once
#include <windows.h>

typedef DWORD (WINAPI *rr_thread_proc)(LPVOID);

#ifdef __cplusplus
extern "C" {
#endif

#ifdef RR_INTERNAL
HANDLE rr_thread_start(rr_thread_proc proc, LPVOID arg);
#else
static __inline HANDLE rr_thread_start(rr_thread_proc proc, LPVOID arg) {
    return CreateThread(NULL, 0, proc, arg, 0, NULL);
}
#endif

#ifdef __cplusplus
}
#endif
