/* rr_int_mem.c - In-process implementation of the rr_mem.h transport.
 *
 * Same API the external client uses, backed by this process's own address
 * space: reads use the Windows memory API, module lookups come from the loader, and
 * the driver entry points report the transport as already open. Compiled into
 * the mapped module; nothing here touches SmxRing.
 */

#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "memory/rr_mem.h"

uint32_t g_rr_pid;
long g_batch_status, g_batch_info, g_batch_expected;

static volatile LONG g_batch_ok;
static volatile LONG g_live;
static uint64_t g_image_base;

static int guarded_write(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void rr_drv_use_replacement(void) {}
int rr_drv_open(void) { return 1; }
int rr_drv_probe(void) { return 1; }
void rr_drv_close(void) {}
int rr_drv_live(void) { return 1; }
int rr_drv_ready(void) { return 1; }
int rr_drv_hide_process(uint32_t pid, int hide) { (void)pid; (void)hide; return 1; }
long rr_drv_get_context(uint32_t thread_id, RR_THREAD_CTX* out) {
    (void)thread_id;
    if (out) memset(out, 0, sizeof(*out));
    return 1;
}

int rr_read(uint64_t addr, void* out, size_t n) {
    if (!addr || !out || !n || n > 0x100000) { rr_stat_reset(); return 0; }
    SIZE_T copied = 0;
    int ok = ReadProcessMemory(GetCurrentProcess(), (const void*)(uintptr_t)addr,
                               out, n, &copied) && copied == n;
    if (ok) InterlockedExchange(&g_live, 1);
    return ok;
}

int rr_read_batch(RR_RD_REQ* items, int n) {
    if (!items || n <= 0) return 0;
    int copied = 0;
    for (int i = 0; i < n; i++) {
        if (items[i].size && items[i].size <= 0x100000 &&
            rr_read(items[i].addr, items[i].dst, items[i].size))
            ++copied;
    }
    InterlockedExchange(&g_batch_ok, copied == n);
    return copied;
}

int rr_write(uint64_t addr, const void* in, size_t n) {
    if (!addr || !in || !n || n > 0x100000) return 0;
    DWORD old = 0;
    if (!VirtualProtect((void*)(uintptr_t)addr, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    int ok = guarded_write((void*)(uintptr_t)addr, in, n);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)addr, n);
    DWORD ignored = 0;
    VirtualProtect((void*)(uintptr_t)addr, n, old, &ignored);
    return ok;
}

int rr_query_region(uint64_t addr, RR_REGION* out) {
    if (!addr || !out) return 0;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((const void*)(uintptr_t)addr, &info, sizeof(info))) return 0;
    out->base = (uint64_t)(uintptr_t)info.BaseAddress;
    out->size = (uint64_t)info.RegionSize;
    out->state = info.State;
    out->protect = info.Protect;
    out->type = info.Type;
    return 1;
}

int rr_drv_alloc_remote(uint32_t pid, size_t size, uint32_t protect, uint64_t* out) {
    (void)pid;
    if (!out || !size || size > 0x2000000u) return 0;
    void* mem = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, protect);
    if (!mem) return 0;
    *out = (uint64_t)(uintptr_t)mem;
    return 1;
}

/* Small reads keep a success flag next to the value so callers can separate
 * "read zero" from "read failed". */
static RR_THREAD_LOCAL int t_read_state;

static void* small_read(uint64_t addr, size_t n, void* dst) {
    t_read_state = rr_read(addr, dst, n);
    return t_read_state ? dst : NULL;
}

uint64_t rr_u64(uint64_t addr) { uint64_t v = 0; small_read(addr, 8, &v); return v; }
uint32_t rr_u32(uint64_t addr) { uint32_t v = 0; small_read(addr, 4, &v); return v; }
int32_t rr_i32(uint64_t addr) { int32_t v = 0; small_read(addr, 4, &v); return v; }
float rr_f32(uint64_t addr) { float v = 0; small_read(addr, 4, &v); return v; }
int rr_ok(void) { return t_read_state; }

int rr_attach(const char* exe_name) {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!len || len >= MAX_PATH) return 0;
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    if (exe_name && *exe_name && _stricmp(base, exe_name) != 0) return 0;
    g_rr_pid = GetCurrentProcessId();
    return 1;
}

int rr_process_visible(uint32_t pid) {
    if (!pid) return 0;
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!proc) return GetLastError() == ERROR_INVALID_PARAMETER ? 0 : -1;
    CloseHandle(proc);
    return 1;
}

uint64_t rr_image_base(void) {
    if (!g_image_base) g_image_base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    return g_image_base;
}

uint64_t rr_peb(void) {
    return __readgsqword(0x60); /* GS:[0x60] is this thread's PEB. */
}

int rr_module(const char* name, uint64_t* base, uint32_t* size) {
    *base = 0;
    if (size) *size = 0;
    HMODULE mod = GetModuleHandleA(name);
    if (!mod) return 0;
    *base = (uint64_t)(uintptr_t)mod;
    if (size) {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)mod;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)mod + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                *size = nt->OptionalHeader.SizeOfImage;
        }
        if (!*size) {
            MODULEINFO info;
            if (GetModuleInformation(GetCurrentProcess(), mod, &info, sizeof(info)))
                *size = info.SizeOfImage;
        }
    }
    return *base != 0;
}

int rr_modules(char* names, uint64_t* bases, uint32_t* sizes, int max) {
    if (!names || !bases || max <= 0) return 0;
    HMODULE mods[256];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return 0;
    int total = (int)(needed / sizeof(HMODULE));
    if (total > (int)(sizeof(mods) / sizeof(mods[0]))) total = (int)(sizeof(mods) / sizeof(mods[0]));
    int n = 0;
    for (int i = 0; i < total && n < max; i++) {
        char path[MAX_PATH];
        if (!GetModuleFileNameA(mods[i], path, MAX_PATH)) continue;
        const char* base = path;
        for (const char* p = path; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        snprintf(names + (size_t)n * 64, 64, "%s", base);
        bases[n] = (uint64_t)(uintptr_t)mods[i];
        if (sizes) {
            MODULEINFO info;
            sizes[n] = GetModuleInformation(GetCurrentProcess(), mods[i], &info, sizeof(info))
                           ? info.SizeOfImage : 0;
        }
        ++n;
    }
    return n;
}

int rr_scan(uint64_t start, uint64_t size, const uint8_t* pat, const char* mask,
            uint64_t* hits, int max_hits) {
    if (!start || !size || !pat || !mask || !hits || max_hits <= 0) return 0;
    size_t len = strlen(mask);
    if (!len || len > size) return 0;

    const size_t page = 0x1000;
    uint8_t page_buf[0x1000];
    int found = 0;
    /* A heap region can hold guard or decommitted pages. */
    for (size_t off = 0; off + len <= size && found < max_hits; ) {
        size_t chunk = page - (size_t)((start + off) & (page - 1));
        if (chunk > size - off) chunk = size - off;
        if (!rr_read(start + off, page_buf, chunk)) { off += chunk; continue; }
        for (size_t i = 0; i + len <= chunk && found < max_hits; i++) {
            int matched = 1;
            for (size_t k = 0; k < len; k++) {
                if (mask[k] == 'x' && page_buf[i + k] != pat[k]) { matched = 0; break; }
            }
            if (matched) hits[found++] = start + off + (uint64_t)i;
        }
        off += chunk;
    }
    return found;
}

long rr_stat_calls(void) { return 0; }
long rr_stat_bytes(void) { return 0; }
void rr_stat_reset(void) { t_read_state = 0; }
