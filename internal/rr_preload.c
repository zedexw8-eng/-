/* rr_preload.c - Suspended-launch stub that LoadLibraryA's a module before
 * the title initializes its anti-cheat.
 *
 * Cross-process rights on a live protected title are stripped by the
 * kernel (VirtualAllocEx / WriteProcessMemory / thread creation all return
 * access denied), but a process we create ourselves carries no such
 * protection until it runs. The stub therefore does the only two things
 * that must happen in-process: wait for the loader to be free, then call
 * LoadLibraryA. Addresses for kernel32!Sleep and kernel32!LoadLibraryA are
 * computed locally and translated by module offset; system DLLs share one
 * base across processes for the life of the boot, and the base is verified
 * readable in the target before anything is staged.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rr_preload.h"
#include "rr_map.h"

static int pre_fail(char* err, size_t cap, const char* fmt, ...) {
    if (err && cap) {
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(err, cap, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static void put_imm64(BYTE** p, BYTE opcode1, BYTE opcode2, uint64_t value) {
    *(*p)++ = opcode1;
    *(*p)++ = opcode2;
    memcpy(*p, &value, sizeof(value));
    *p += sizeof(value);
}

/* sub rsp,40; mov rcx,delay; mov rax,Sleep; call rax; mov rcx,path;
 * mov rax,LoadLibraryA; call rax; add rsp,40; xor eax,eax; ret */
static size_t build_stub(BYTE* out, uint64_t sleep_fn, uint64_t load_fn,
                         unsigned delay_ms, uint64_t path_addr) {
    BYTE* p = out;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x28;   /* sub rsp, 40 */
    put_imm64(&p, 0x48, 0xB9, delay_ms);                  /* mov rcx, delay */
    put_imm64(&p, 0x48, 0xB8, sleep_fn);                  /* mov rax, Sleep */
    *p++ = 0xFF; *p++ = 0xD0;                             /* call rax */
    put_imm64(&p, 0x48, 0xB9, path_addr);                 /* mov rcx, path */
    put_imm64(&p, 0x48, 0xB8, load_fn);                   /* mov rax, LoadLibraryA */
    *p++ = 0xFF; *p++ = 0xD0;                             /* call rax */
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x28;   /* add rsp, 40 */
    *p++ = 0x31; *p++ = 0xC0;                             /* xor eax, eax */
    *p++ = 0xC3;                                          /* ret */
    return (size_t)(p - out);
}

int rr_preload_dll(const char* game_exe, const char* dll_path, unsigned delay_ms,
                   RR_PRELOAD_RESULT* out, char* err, size_t err_cap) {
    if (out) { out->pid = 0; out->thread_id = 0; }
    if (!game_exe || !*game_exe || !dll_path || !*dll_path)
        return pre_fail(err, err_cap, "rr_preload: missing game exe or module path");
    if (GetFileAttributesA(game_exe) == INVALID_FILE_ATTRIBUTES)
        return pre_fail(err, err_cap, "rr_preload: '%s' not found", game_exe);
    if (GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES)
        return pre_fail(err, err_cap, "rr_preload: '%s' not found", dll_path);

    char dll_full[MAX_PATH];
    if (!GetFullPathNameA(dll_path, MAX_PATH, dll_full, NULL))
        return pre_fail(err, err_cap, "rr_preload: cannot resolve '%s'", dll_path);

    char workdir[MAX_PATH];
    snprintf(workdir, sizeof(workdir), "%s", game_exe);
    char* slash = strrchr(workdir, '\\');
    if (slash) *slash = 0; else workdir[0] = 0;   /* the title needs its data dir */

    char cmd[MAX_PATH + 8];
    snprintf(cmd, sizeof(cmd), "\"%s\"", game_exe);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0,
                        NULL, workdir[0] ? workdir : NULL, &si, &pi))
        return pre_fail(err, err_cap, "rr_preload: cannot start '%s' (Win32 %lu)",
                        game_exe, (unsigned long)GetLastError());

    int ok = 0;
    void* page = NULL;
    HANDLE thread = NULL;

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    uint64_t sleep_local = k32 ? (uint64_t)(uintptr_t)GetProcAddress(k32, "Sleep") : 0;
    uint64_t load_local = k32 ? (uint64_t)(uintptr_t)GetProcAddress(k32, "LoadLibraryA") : 0;
    if (!k32 || !sleep_local || !load_local) {
        pre_fail(err, err_cap, "rr_preload: kernel32 exports are unavailable");
        goto done;
    }
    uint64_t k32_base = (uint64_t)(uintptr_t)k32;

    /* The loader list only exists once ntdll has initialised the process, so
     * poll for kernel32 instead of reading it from a suspended image. Bases
     * are randomised per process, hence the target's own LDR entry. */
    uint64_t k32_remote = 0;
    for (int attempt = 0; attempt < 600 && !k32_remote; attempt++) {
        uint64_t peb = rr_remote_peb(pi.hProcess);
        if (peb && rr_remote_module(pi.hProcess, peb, "kernel32.dll", &k32_remote) &&
            k32_remote)
            break;
        if (WaitForSingleObject(pi.hProcess, 5) == WAIT_OBJECT_0) break;  /* it died */
    }
    if (!k32_remote) {
        pre_fail(err, err_cap, "rr_preload: the title never exposed its loader list");
        goto done;
    }

    IMAGE_DOS_HEADER dos;
    SIZE_T got = 0;
    if (!ReadProcessMemory(pi.hProcess, (LPCVOID)(uintptr_t)k32_remote, &dos, sizeof(dos), &got) ||
        got != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        pre_fail(err, err_cap, "rr_preload: kernel32 header is unreadable in the title");
        goto done;
    }
    sleep_local += k32_remote - k32_base;
    load_local += k32_remote - k32_base;

    page = VirtualAllocEx(pi.hProcess, NULL, 0x1000, MEM_COMMIT | MEM_RESERVE,
                          PAGE_EXECUTE_READWRITE);
    if (!page) {
        pre_fail(err, err_cap, "rr_preload: VirtualAllocEx failed (Win32 %lu)",
                 (unsigned long)GetLastError());
        goto done;
    }

    const uint64_t page_addr = (uint64_t)(uintptr_t)page;
    const uint64_t path_addr = page_addr + 0x100;
    BYTE stub[128];
    size_t stub_len = build_stub(stub, sleep_local, load_local, delay_ms, path_addr);

    if (!WriteProcessMemory(pi.hProcess, page, stub, stub_len, NULL)) {
        pre_fail(err, err_cap, "rr_preload: cannot stage the stub (Win32 %lu)",
                 (unsigned long)GetLastError());
        goto done;
    }
    size_t dll_len = strlen(dll_full) + 1;
    if (dll_len > 0x800 ||
        !WriteProcessMemory(pi.hProcess, (LPVOID)(uintptr_t)path_addr, dll_full, dll_len, NULL)) {
        pre_fail(err, err_cap, "rr_preload: cannot write the module path (Win32 %lu)",
                 (unsigned long)GetLastError());
        goto done;
    }

    thread = CreateRemoteThread(pi.hProcess, NULL, 0,
                                (LPTHREAD_START_ROUTINE)page, NULL, 0, NULL);
    if (!thread) {
        pre_fail(err, err_cap,
                 "rr_preload: thread creation refused (Win32 %lu)",
                 (unsigned long)GetLastError());
        goto done;
    }

    if (out) { out->pid = pi.dwProcessId; out->thread_id = GetThreadId(thread); }
    ok = 1;

done:
    if (thread) CloseHandle(thread);
    if (!ok && page) VirtualFreeEx(pi.hProcess, page, 0, MEM_RELEASE);
    if (!ok && pi.hProcess) TerminateProcess(pi.hProcess, 1);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    return ok;
}
