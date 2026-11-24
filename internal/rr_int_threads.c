/* rr_int_threads.c - rr_thread_start for the in-process build.
 *
 * Every job that used to call CreateThread now becomes a thread-pool work
 * item on the default pool. ntdll owns the worker threads, so no ETHREAD in
 * the process carries a Win32StartAddress pointing at the mapped image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "game/rr_threads.h"

typedef struct rr_job {
    rr_thread_proc proc;
    LPVOID arg;
    HANDLE done;
    PTP_WORK work;
} rr_job;

static VOID CALLBACK rr_job_run(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work) {
    (void)instance;
    rr_job* job = (rr_job*)context;
    job->proc(job->arg);
    CloseThreadpoolWork(work);
    SetEvent(job->done);
    CloseHandle(job->done);
    free(job);
}

HANDLE rr_thread_start(rr_thread_proc proc, LPVOID arg) {
    if (!proc) return NULL;

    rr_job* job = (rr_job*)calloc(1, sizeof(rr_job));
    if (!job) return NULL;

    job->proc = proc;
    job->arg = arg;
    job->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!job->done) {
        free(job);
        return NULL;
    }
    HANDLE completion = NULL;
    HANDLE self = GetCurrentProcess();
    if (!DuplicateHandle(self, job->done, self, &completion, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        CloseHandle(job->done);
        free(job);
        return NULL;
    }

    job->work = CreateThreadpoolWork(rr_job_run, job, NULL);
    if (!job->work) {
        CloseHandle(completion);
        CloseHandle(job->done);
        free(job);
        return NULL;
    }

    SubmitThreadpoolWork(job->work);
    return completion;
}
