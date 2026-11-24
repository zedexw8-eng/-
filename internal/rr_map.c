/* rr_map.c - Manual PE64 mapping for the in-process client module.
 *
 * The Windows loader is never invoked: no LoadLibraryA in the target, no
 * CreateRemoteThread. Each remote code execution (TLS callbacks, exception
 * table registration, entry point) runs on an already-running target thread
 * that is suspended, redirected to a stub, and put back with its saved
 * context.
 *
 * Offsets used for the remote loader walk are the same x64 layout the memory
 * reader relies on: PEB+0x18 Ldr, Ldr+0x10 InLoadOrder head, entry+0x00
 * Flink, +0x30 DllBase, +0x40 SizeOfImage, +0x58/0x60 BaseDllName length
 * and buffer.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "rr_map.h"

#define MAP_MAX_FILE   (64u * 1024u * 1024u)
#define MAP_MAX_IMAGE  (256u * 1024u * 1024u)
#define MAP_MAX_THUNKS 65536

/* Set RR_MAP_TRACE=1 to append stage-by-stage progress to map_trace.log. */
static void map_trace(const char* fmt, ...) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("RR_MAP_TRACE");
        enabled = v && *v == '1';
        enabled = 1; /* TEMP debug */
    }
    if (!enabled) return;
    FILE* f = fopen("map_trace.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

typedef struct MOD_NAME {
    char name[64];
    uint64_t remote_base;
} MOD_NAME;

static int map_fail(char* err, size_t cap, const char* fmt, ...) {
    if (err && cap) {
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(err, cap, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static int rd(HANDLE proc, uint64_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    if (!n) return 1;
    if (!ReadProcessMemory(proc, (LPCVOID)(uintptr_t)addr, out, n, &got)) return 0;
    return got == n;
}

static int wr(HANDLE proc, uint64_t addr, const void* in, size_t n) {
    SIZE_T put = 0;
    if (!n) return 1;
    if (!WriteProcessMemory(proc, (LPVOID)(uintptr_t)addr, in, n, &put)) return 0;
    return put == n;
}

static uint32_t rd32(HANDLE proc, uint64_t addr) {
    uint32_t v = 0;
    if (!rd(proc, addr, &v, sizeof(v))) return 0;
    return v;
}

static uint64_t rd64(HANDLE proc, uint64_t addr) {
    uint64_t v = 0;
    if (!rd(proc, addr, &v, sizeof(v))) return 0;
    return v;
}

static int plausible(uint64_t v) {
    return v >= 0x10000ULL && v < 0x00007FFFFFFFFFFFULL;
}

static int read_file(const char* path, BYTE** out, DWORD* size, char* err, size_t cap) {
    wchar_t wide[MAX_PATH * 2];
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH * 2) == 0)
        return map_fail(err, cap, "rr_map: cannot widen '%s'", path);
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return map_fail(err, cap, "rr_map: cannot open '%s' (Win32 %lu)", path,
                        (unsigned long)GetLastError());
    LARGE_INTEGER len;
    if (!GetFileSizeEx(file, &len) || len.QuadPart < 0 ||
        (uint64_t)len.QuadPart > MAP_MAX_FILE) {
        CloseHandle(file);
        return map_fail(err, cap, "rr_map: '%s' is empty or larger than %u MiB",
                        path, MAP_MAX_FILE >> 20);
    }
    DWORD want = (DWORD)len.QuadPart;
    BYTE* data = (BYTE*)malloc(want);
    if (!data) {
        CloseHandle(file);
        return map_fail(err, cap, "rr_map: out of memory for '%s'", path);
    }
    DWORD got = 0;
    BOOL ok = ReadFile(file, data, want, &got, NULL);
    CloseHandle(file);
    if (!ok || got != want) {
        free(data);
        return map_fail(err, cap, "rr_map: short read on '%s'", path);
    }
    *out = data;
    *size = want;
    return 1;
}

/* Remote PEB through NtQueryInformationProcess: no driver, no remote thread. */
uint64_t rr_remote_peb(HANDLE proc) {
    typedef LONG (WINAPI* FN_NQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static FN_NQIP nqip;
    if (!nqip) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) nqip = (FN_NQIP)(void*)GetProcAddress(ntdll, "NtQueryInformationProcess");
    }
    if (!nqip) return 0;
    /* PROCESS_BASIC_INFORMATION is 6 pointer-sized fields; a short buffer is
     * rejected with STATUS_INFO_LENGTH_MISMATCH. */
    struct {
        PVOID reserved1;
        PVOID peb;
        PVOID reserved2[2];
        ULONG_PTR unique_pid;
        ULONG_PTR reserved3;
    } info;
    memset(&info, 0, sizeof(info));
    ULONG ret = 0;
    if (nqip(proc, 0 /* ProcessBasicInformation */, &info, sizeof(info), &ret) != 0)
        return 0;
    uint64_t peb = (uint64_t)(uintptr_t)info.peb;
    return plausible(peb) ? peb : 0;
}

int rr_remote_module(HANDLE proc, uint64_t peb, const char* name, uint64_t* base) {
    *base = 0;
    uint64_t ldr = rd64(proc, peb + 0x18);
    if (!plausible(ldr)) return 0;
    uint64_t head = ldr + 0x10;
    uint64_t cur = rd64(proc, head);
    if (!plausible(cur)) return 0;

    for (int i = 0; i < 512; i++) {
        if (cur == head || !plausible(cur)) break;
        USHORT len = (USHORT)rd32(proc, cur + 0x58);
        uint64_t buf = rd64(proc, cur + 0x60);
        if (len >= 2 && len < 512 && plausible(buf)) {
            wchar_t got[256];
            size_t chars = len / 2;
            if (chars > 255) chars = 255;
            if (rd(proc, buf, got, chars * 2)) {
                wchar_t want[64];
                got[chars] = 0;
                if (MultiByteToWideChar(CP_ACP, 0, name, -1, want, 64) != 0 &&
                    _wcsicmp(got, want) == 0) {
                    uint64_t dll_base = rd64(proc, cur + 0x30);
                    if (!plausible(dll_base)) return 0;
                    *base = dll_base;
                    return 1;
                }
            }
        }
        cur = rd64(proc, cur);
    }
    return 0;
}

/* Local module that owns an address, by snapshot of our own loader list. */
static int local_owner(void* addr, char* name, size_t name_cap,
                       uint64_t* base_out, uint32_t* size_out) {
    uintptr_t p = (uintptr_t)addr;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    BOOL more = Module32FirstW(snap, &entry);
    while (more) {
        uintptr_t mod_base = (uintptr_t)entry.modBaseAddr;
        if (p >= mod_base && p < mod_base + entry.modBaseSize) {
            WideCharToMultiByte(CP_ACP, 0, entry.szModule, -1, name, (int)name_cap, NULL, NULL);
            *base_out = (uint64_t)mod_base;
            *size_out = entry.modBaseSize;
            CloseHandle(snap);
            return 1;
        }
        more = Module32NextW(snap, &entry);
    }
    CloseHandle(snap);
    return 0;
}

static FARPROC local_export(const char* module, const uint64_t* ordinals,
                            const char* name, int by_ordinal) {
    HMODULE mod = LoadLibraryA(module);
    if (!mod) return NULL;
    if (by_ordinal) return GetProcAddress(mod, (LPCSTR)(uintptr_t)*ordinals);
    return GetProcAddress(mod, name);
}

static int remote_call(HANDLE proc, uint64_t page, uint64_t fn, uint64_t a1,
                       uint64_t a2, uint64_t a3, DWORD* used_tid,
                       const char* stage, char* err, size_t cap);

/* A module the target has not loaded yet (d3dcompiler_47 is the common one
 * for an ImGui DX11 backend, Unity never pulls it) still has to exist for
 * the import table. Load it through the target's own kernel32 before the
 * binding pass gives up. */
static int preload_module(HANDLE proc, uint64_t peb, uint64_t stub_page,
                          const char* name, uint64_t* base) {
    static uint64_t load_library_w;   /* local address, resolved once */
    HMODULE local_k32 = GetModuleHandleW(L"kernel32.dll");
    if (!local_k32) return 0;
    if (!load_library_w)
        load_library_w = (uint64_t)(uintptr_t)GetProcAddress(local_k32, "LoadLibraryW");
    if (!load_library_w) return 0;

    uint64_t remote_k32 = 0;
    if (!rr_remote_module(proc, peb, "kernel32.dll", &remote_k32)) return 0;
    uint64_t fn_remote = remote_k32 + (load_library_w - (uint64_t)(uintptr_t)local_k32);

    wchar_t wide[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH) == 0) return 0;

    const uint64_t arg_off = 0x200;   /* clear of the 64-byte call stub */
    if (!wr(proc, stub_page + arg_off, wide, sizeof(wide))) return 0;
    if (!remote_call(proc, stub_page, fn_remote, stub_page + arg_off, 0, 0,
                     NULL, "LoadLibraryW", NULL, 0))
        return 0;
    /* The stub discards the HMODULE; the loader list is the proof. */
    return rr_remote_module(proc, peb, name, base);
}

/* Import binding: the export's RVA comes from the local copy of the same
 * binary, the base from the target's loader list. Forwarded exports are
 * followed into the module that actually holds the code. */
static int bind_imports(HANDLE proc, uint64_t peb, uint64_t stub_page, BYTE* image,
                        const IMAGE_DATA_DIRECTORY* dir, MOD_NAME* cache,
                        int* cache_n, char* err, size_t cap) {
    if (!dir->VirtualAddress || dir->Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return 1;
    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(image + dir->VirtualAddress);

    for (int d = 0; desc[d].Name && d < 512; d++) {
        const char* module = (const char*)image + desc[d].Name;
        uint64_t remote_base = 0;
        for (int c = 0; c < *cache_n; c++) {
            if (_stricmp(cache[c].name, module) == 0) {
                remote_base = cache[c].remote_base;
                break;
            }
        }
        if (!remote_base) {
            if (!rr_remote_module(proc, peb, module, &remote_base) &&
                !preload_module(proc, peb, stub_page, module, &remote_base))
                return map_fail(err, cap, "rr_map: import module '%s' is not loaded in the target",
                                module);
            if (*cache_n < 32) {
                snprintf(cache[*cache_n].name, sizeof(cache[0].name), "%s", module);
                cache[*cache_n].remote_base = remote_base;
                ++*cache_n;
            }
        }
        HMODULE local = LoadLibraryA(module);
        if (!local)
            return map_fail(err, cap, "rr_map: cannot load a local copy of '%s'", module);

        uint64_t* thunk = (uint64_t*)(image + (desc[d].OriginalFirstThunk ?
                                               desc[d].OriginalFirstThunk : desc[d].FirstThunk));
        uint64_t* iat = (uint64_t*)(image + desc[d].FirstThunk);
        for (int t = 0; t < MAP_MAX_THUNKS && thunk[t]; t++) {
            int by_ordinal = (thunk[t] & IMAGE_ORDINAL_FLAG64) != 0;
            uint64_t ordinal = thunk[t] & 0xFFFF;
            const char* fn_name = by_ordinal ? NULL :
                (const char*)(image + (uint32_t)thunk[t] + 2);
            FARPROC fn = local_export(module, &ordinal, fn_name, by_ordinal);
            if (!fn)
                return map_fail(err, cap, "rr_map: cannot resolve %s!%s",
                                module, by_ordinal ? "#ordinal" : fn_name);

            char owner[64];
            uint64_t owner_base = 0;
            uint32_t owner_size = 0;
            if (!local_owner((void*)fn, owner, sizeof(owner), &owner_base, &owner_size))
                return map_fail(err, cap, "rr_map: cannot locate the module owning %s!%s",
                                module, by_ordinal ? "#ordinal" : fn_name);

            uint64_t owner_remote = remote_base;
            if (_stricmp(owner, module) != 0) {
                int hit = 0;
                for (int c = 0; c < *cache_n; c++)
                    if (_stricmp(cache[c].name, owner) == 0) { owner_remote = cache[c].remote_base; hit = 1; break; }
                if (!hit) {
                    if (!rr_remote_module(proc, peb, owner, &owner_remote) &&
                        !preload_module(proc, peb, stub_page, owner, &owner_remote))
                        return map_fail(err, cap, "rr_map: forwarded module '%s' is not loaded in the target", owner);
                    if (*cache_n < 32) {
                        snprintf(cache[*cache_n].name, sizeof(cache[0].name), "%s", owner);
                        cache[*cache_n].remote_base = owner_remote;
                        ++*cache_n;
                    }
                }
            }
            uint64_t rva = (uint64_t)fn - owner_base;
            if (rva >= owner_size)
                return map_fail(err, cap, "rr_map: export %s!%s fell outside its module",
                                module, by_ordinal ? "#ordinal" : fn_name);
            iat[t] = owner_remote + rva;
        }
    }
    return 1;
}

static void apply_relocations(BYTE* image, const IMAGE_DATA_DIRECTORY* dir,
                              const IMAGE_NT_HEADERS64* nt, uint64_t delta) {
    if (!delta || !dir->VirtualAddress || !dir->Size) return;
    DWORD off = 0;
    while (off + sizeof(IMAGE_BASE_RELOCATION) <= dir->Size) {
        IMAGE_BASE_RELOCATION* block =
            (IMAGE_BASE_RELOCATION*)(image + dir->VirtualAddress + off);
        if (!block->SizeOfBlock || block->SizeOfBlock > dir->Size - off) break;
        DWORD entries = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        WORD* list = (WORD*)((BYTE*)block + sizeof(IMAGE_BASE_RELOCATION));
        for (DWORD i = 0; i < entries; i++) {
            WORD type = (WORD)(list[i] >> 12);
            DWORD where = block->VirtualAddress + (list[i] & 0x0FFF);
            if (where + 8 <= nt->OptionalHeader.SizeOfImage && type == IMAGE_REL_BASED_DIR64) {
                uint64_t* slot = (uint64_t*)(image + where);
                *slot += delta;
            } else if (where + 4 <= nt->OptionalHeader.SizeOfImage && type == IMAGE_REL_BASED_HIGHLOW) {
                uint32_t* slot = (uint32_t*)(image + where);
                *slot += (uint32_t)delta;
            }
            /* IMAGE_REL_BASED_ABSOLUTE entries carry no patch. */
        }
        off += block->SizeOfBlock;
    }
}

static DWORD section_protect(DWORD characteristics) {
    DWORD prot = PAGE_READONLY;
    if (characteristics & IMAGE_SCN_MEM_EXECUTE) {
        if (characteristics & IMAGE_SCN_MEM_WRITE) prot = PAGE_EXECUTE_READWRITE;
        else if (characteristics & IMAGE_SCN_MEM_READ) prot = PAGE_EXECUTE_READ;
        else prot = PAGE_EXECUTE;
    } else if (characteristics & IMAGE_SCN_MEM_WRITE) {
        prot = PAGE_READWRITE;
    }
    return prot;
}

/* Thread-start stub: fn(a1,a2,a3) then return, so the helper thread exits on
 * its own and never needs TerminateThread. */
static size_t build_call_stub(BYTE* out, uint64_t fn, uint64_t a1, uint64_t a2,
                              uint64_t a3) {
    size_t n = 0;
    out[n++] = 0x48; out[n++] = 0xB8; memcpy(out + n, &fn, 8); n += 8;    /* mov rax, fn */
    out[n++] = 0x48; out[n++] = 0xB9; memcpy(out + n, &a1, 8); n += 8;    /* mov rcx, a1 */
    out[n++] = 0x48; out[n++] = 0xBA; memcpy(out + n, &a2, 8); n += 8;    /* mov rdx, a2 */
    out[n++] = 0x49; out[n++] = 0xB8; memcpy(out + n, &a3, 8); n += 8;    /* mov r8,  a3 */
    out[n++] = 0x48; out[n++] = 0x83; out[n++] = 0xEC; out[n++] = 0x28;   /* sub rsp, 40 */
    out[n++] = 0xFF; out[n++] = 0xD0;                                     /* call rax */
    out[n++] = 0x48; out[n++] = 0x83; out[n++] = 0xC4; out[n++] = 0x28;   /* add rsp, 40 */
    out[n++] = 0x31; out[n++] = 0xC0;                                     /* xor eax, eax */
    out[n++] = 0xC3;                                                      /* ret */
    return n;
}

typedef LONG (NTAPI* FN_NCTE)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID,
                              PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);

/* Runs fn(a1,a2,a3) in a short-lived target thread. NtCreateThreadEx is
 * resolved directly: the kernel32 wrappers are the parts anti-cheat hooks
 * first, and no LoadLibraryA is involved at any point. A thread parked in a
 * kernel wait can swallow a context rewrite, which is why this does not
 * borrow an existing thread. */
static int remote_call(HANDLE proc, uint64_t page, uint64_t fn, uint64_t a1,
                       uint64_t a2, uint64_t a3, DWORD* used_tid,
                       const char* stage, char* err, size_t cap) {
    static FN_NCTE ncte;
    if (!ncte) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) ncte = (FN_NCTE)(void*)GetProcAddress(ntdll, "NtCreateThreadEx");
        if (!ncte)
            return map_fail(err, cap, "rr_map: NtCreateThreadEx is unavailable");
    }

    BYTE stub[64];
    size_t stub_len = build_call_stub(stub, fn, a1, a2, a3);
    if (!wr(proc, page, stub, stub_len))
        return map_fail(err, cap, "rr_map: cannot stage the call stub (Win32 %lu)",
                        (unsigned long)GetLastError());

    map_trace("call %s fn=%llX args=%llX,%llX,%llX", stage,
              (unsigned long long)fn, (unsigned long long)a1,
              (unsigned long long)a2, (unsigned long long)a3);

    HANDLE thread = NULL;
    LONG status = ncte(&thread, THREAD_ALL_ACCESS, NULL, proc, (PVOID)(uintptr_t)page,
                       NULL, 0, 0, 0, 0, NULL);
    if (status < 0 || !thread)
        return map_fail(err, cap, "rr_map: %s thread creation failed (NTSTATUS %08lX)",
                        stage, (unsigned long)status);

    DWORD tid = GetThreadId(thread);
    DWORD wait = WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    map_trace("  %s %s tid=%lu", stage, wait == WAIT_OBJECT_0 ? "returned" : "TIMED OUT",
              (unsigned long)tid);

    if (wait != WAIT_OBJECT_0)
        return map_fail(err, cap,
                        "rr_map: %s did not finish within 5 s (thread %lu left running)",
                        stage, (unsigned long)tid);
    if (used_tid) *used_tid = tid;
    return 1;
}

int rr_map_dll(uint32_t pid, const char* dll_path, RR_MAP_RESULT* out,
               char* err, size_t err_cap) {
    if (out) { out->base = 0; out->size = 0; out->thread_id = 0; }
    if (!pid || !dll_path || !*dll_path)
        return map_fail(err, err_cap, "rr_map: missing pid or module path");

    BYTE* file = NULL;
    DWORD file_size = 0;
    if (!read_file(dll_path, &file, &file_size, err, err_cap)) return 0;

    map_trace("---- rr_map_dll pid=%u file=%s", pid, dll_path);
    int ok = 0;
    BYTE* image = NULL;
    uint64_t remote_image = 0;
    uint64_t stub_page = 0;
    HANDLE proc = NULL;

    if (file_size < sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS64)) {
        map_fail(err, err_cap, "rr_map: '%s' is not a PE image", dll_path);
        goto done;
    }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)file;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        (DWORD)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > file_size) {
        map_fail(err, err_cap, "rr_map: '%s' has a broken DOS/NT header", dll_path);
        goto done;
    }
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(file + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(nt->FileHeader.Characteristics & IMAGE_FILE_DLL)) {
        map_fail(err, err_cap, "rr_map: '%s' is not an x64 DLL", dll_path);
        goto done;
    }
    DWORD image_size = nt->OptionalHeader.SizeOfImage;
    if (image_size < nt->OptionalHeader.SizeOfHeaders || image_size > MAP_MAX_IMAGE ||
        nt->FileHeader.NumberOfSections > 96) {
        map_fail(err, err_cap, "rr_map: '%s' has an implausible SizeOfImage (%lu)",
                 dll_path, (unsigned long)image_size);
        goto done;
    }

    image = (BYTE*)VirtualAlloc(NULL, image_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!image) {
        map_fail(err, err_cap, "rr_map: out of memory for the '%s' image", dll_path);
        goto done;
    }

    proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                       PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                       PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!proc) {
        map_fail(err, err_cap, "rr_map: OpenProcess(%u) failed (Win32 %lu)", pid,
                 (unsigned long)GetLastError());
        goto done;
    }

    uint64_t peb = rr_remote_peb(proc);
    if (!peb) {
        map_fail(err, err_cap, "rr_map: cannot read the target PEB");
        goto done;
    }

    memcpy(image, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD raw = section[i].PointerToRawData;
        DWORD raw_size = section[i].SizeOfRawData;
        DWORD virtual_size = section[i].Misc.VirtualSize;
        DWORD copy = raw_size < virtual_size ? raw_size : virtual_size;
        if (!copy) continue;
        if (raw + copy > file_size ||
            (uint64_t)section[i].VirtualAddress + copy > image_size) {
            map_fail(err, err_cap, "rr_map: section %d of '%s' runs past its image",
                     (int)i, dll_path);
            goto done;
        }
        memcpy(image + section[i].VirtualAddress, file + raw, copy);
    }

    /* Preferred base first; anywhere if it is taken, with full relocation. */
    remote_image = (uint64_t)(uintptr_t)VirtualAllocEx(
        proc, (LPVOID)(uintptr_t)nt->OptionalHeader.ImageBase, image_size,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_image) {
        remote_image = (uint64_t)(uintptr_t)VirtualAllocEx(
            proc, NULL, image_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote_image) {
            map_fail(err, err_cap, "rr_map: VirtualAllocEx(%lu) failed (Win32 %lu)",
                     (unsigned long)image_size, (unsigned long)GetLastError());
            goto done;
        }
    }

    uint64_t delta = remote_image - nt->OptionalHeader.ImageBase;
    apply_relocations(image, &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC],
                      nt, delta);

    MOD_NAME cache[32];
    int cache_n = 0;
    memset(cache, 0, sizeof(cache));
    /* Needed before import binding: a missing dependency is loaded through a
     * remote call that stages its stub and argument on this page. */
    stub_page = (uint64_t)(uintptr_t)VirtualAllocEx(proc, NULL, 0x1000,
                                                    MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_EXECUTE_READWRITE);
    if (!stub_page) {
        map_fail(err, err_cap, "rr_map: cannot allocate the call stub (Win32 %lu)",
                 (unsigned long)GetLastError());
        goto done;
    }
    if (!bind_imports(proc, peb, stub_page, image,
                      &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT],
                      cache, &cache_n, err, err_cap))
        goto done;

    if (delta && !nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
        map_fail(err, err_cap,
                 "rr_map: '%s' cannot move from its preferred base and that base is taken",
                 dll_path);
        goto done;
    }

    if (!wr(proc, remote_image, image, image_size)) {
        map_fail(err, err_cap, "rr_map: WriteProcessMemory(%lu) failed (Win32 %lu)",
                 (unsigned long)image_size, (unsigned long)GetLastError());
        goto done;
    }

    map_trace("image written at %llX (delta %lld), imports bound",
              (unsigned long long)remote_image, (long long)delta);

    /* TLS callbacks run before the entry point, as the loader does. */
    IMAGE_DATA_DIRECTORY* tls_dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls_dir->VirtualAddress && tls_dir->Size >= sizeof(IMAGE_TLS_DIRECTORY64) &&
        tls_dir->VirtualAddress + sizeof(IMAGE_TLS_DIRECTORY64) <= image_size) {
        IMAGE_TLS_DIRECTORY64* tls = (IMAGE_TLS_DIRECTORY64*)(image + tls_dir->VirtualAddress);
        uint64_t callbacks = tls->AddressOfCallBacks;
        for (int i = 0; i < 32 && plausible(callbacks); i++) {
            uint64_t fn = rd64(proc, callbacks + (uint64_t)i * 8);
            if (!plausible(fn)) break;
            if (!remote_call(proc, stub_page, fn, remote_image, DLL_PROCESS_ATTACH, 0,
                             NULL, "TLS callback", err, err_cap))
                goto done;
        }
    }

    /* Unwind data: without it any exception in the module unwinds wrong.
     * RtlAddFunctionTable is the same one the loader uses for images that
     * arrive without a .pdata mapping already registered. */
    IMAGE_DATA_DIRECTORY* exc_dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exc_dir->VirtualAddress && exc_dir->Size >= 12) {
        DWORD entries = exc_dir->Size / 12;
        HMODULE local_ntdll = GetModuleHandleW(L"ntdll.dll");
        FARPROC local_add = local_ntdll ?
            GetProcAddress(local_ntdll, "RtlAddFunctionTable") : NULL;
        uint64_t ntdll_remote = 0;
        if (!local_add || !rr_remote_module(proc, peb, "ntdll.dll", &ntdll_remote)) {
            map_fail(err, err_cap, "rr_map: cannot locate RtlAddFunctionTable");
            goto done;
        }
        uint64_t fn_remote = ntdll_remote + ((uint64_t)(uintptr_t)local_add -
                                             (uint64_t)(uintptr_t)local_ntdll);
        if (!remote_call(proc, stub_page, fn_remote,
                         remote_image + exc_dir->VirtualAddress, entries, remote_image,
                         NULL, "RtlAddFunctionTable", err, err_cap))
            goto done;
    }

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD prot = section_protect(section[i].Characteristics);
        DWORD old = 0;
        if (!VirtualProtectEx(proc, (LPVOID)(uintptr_t)(remote_image + section[i].VirtualAddress),
                              section[i].Misc.VirtualSize ? section[i].Misc.VirtualSize : 1,
                              prot, &old)) {
            map_fail(err, err_cap, "rr_map: VirtualProtectEx on section %d failed (Win32 %lu)",
                     (int)i, (unsigned long)GetLastError());
            goto done;
        }
    }
    FlushInstructionCache(proc, (LPCVOID)(uintptr_t)remote_image, image_size);

    {
        DWORD tid = 0;
        uint64_t entry = remote_image + nt->OptionalHeader.AddressOfEntryPoint;
        if (!remote_call(proc, stub_page, entry, remote_image, DLL_PROCESS_ATTACH, 0,
                         &tid, "entry point", err, err_cap))
            goto done;
        if (out) {
            out->base = remote_image;
            out->size = image_size;
            out->thread_id = tid;
        }
    }
    ok = 1;

done:
    if (stub_page) VirtualFreeEx(proc, (LPVOID)(uintptr_t)stub_page, 0, MEM_RELEASE);
    if (proc) CloseHandle(proc);
    if (image) VirtualFree(image, 0, MEM_RELEASE);
    free(file);
    return ok;
}
