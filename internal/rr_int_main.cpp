/* rr_int_main.cpp - In-process ESP and menu rendered on the game's swap chain.
 *
 * The module hooks IDXGISwapChain::Present through the interface vtable that
 * dxgi.dll publishes, then draws the existing ESP pipeline (game_snapshot ->
 * draw_esp) and the Daydream menu into the title's own back buffer using the
 * title's D3D11 device. No second window, no composition surface.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_freetype.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "ui/amphetaminehaxx/ui.hpp"
#include "menu/settings/font/font.hpp"
#include "ui/menu_bridge.h"
#include "ui/gpu.h"
#include "game/rr_game.h"
#include "game/rr_threads.h"
#include "features/aim/rr_aim.h"
#include "features/esp/rr_draw.h"
#include "features/visibility/rr_los.h"

using Microsoft::WRL::ComPtr;

static volatile LONG g_menu_visible;
static volatile LONG g_ready;
static volatile LONG g_hook_installed;
static SRWLOCK g_snapshot_lock = SRWLOCK_INIT;
static constexpr int snapshot_capacity = 128;
static RR_PLAYER* g_snapshot_players;
static RR_CAMERA g_snapshot_camera;
static int g_snapshot_count;
static unsigned g_snapshot_generation;
static DWORD g_snapshot_at;
static uint64_t g_snapshot_sequence;

static ID3D11Device* g_device;
static ID3D11DeviceContext* g_context;
static HWND g_window;
static IDXGISwapChain* g_swap;
static UINT g_width = 1920, g_height = 1080;

typedef HRESULT (WINAPI* PRESENT_FN)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (WINAPI* PRESENT1_FN)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PRESENT_FN g_present_original;
static PRESENT1_FN g_present1_original;
static HRESULT WINAPI present_hook(IDXGISwapChain* swap, UINT sync, UINT flags);
static HRESULT WINAPI present1_hook(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params);

/* A flip-model chain hands the screen whichever buffer GetCurrentBackBufferIndex()
 * names, not buffer 0; drawing into 0 while the chain flips to 1 shows the ESP on
 * every other frame. Unity also routes some presents through Present1, which can
 * carry dirty rects that leave the rest of the frame (our ESP) uncopied. */
static volatile LONG g_is_flip;
static int g_buffer_count = 1;
static IDXGISwapChain* g_chain3_owner;
static IDXGISwapChain3* g_chain3;
static unsigned long g_present_n, g_draw_n, g_empty_n, g_buf_fail, g_rtv_fail, g_test_skip;
static int g_last_players;

/* Which part of the render path the present thread is inside. Kept apart from
 * the scene worker's phase so the two threads never overwrite each other. */
static volatile LONG g_rphase;

static HMODULE image_base_of(const void* address);

/* ---- shared settings (the external client keeps these in menu_host.cpp;
 * the internal build carries its own copy so it does not drag in the menu
 * window, its embedded font resource or its own device). ---- */

BR_Config* br_get(void) {
    static BR_Config config = [] {
        BR_Config defaults{};
        defaults.magic = BR_MAGIC;
        defaults.version = BR_VERSION;
        defaults.hud_accent_rgb = 0x5b7db5;
        defaults.hud_cursor_rgb = 0x141414;
        defaults.hud_outline_rgb = 0x000000;
        defaults.hud_panel_rgb = 0x2b2b2b;
        defaults.hud_inl_rgb = 0x383838;
        defaults.hud_dark_rgb = 0x1e1e1e;
        defaults.hud_text_rgb = 0xffffff;
        defaults.hud_muted_rgb = 0xafafaf;
#define BR_DEFAULT(field, initial, minimum, maximum) defaults.field = initial;
        BR_SETTINGS(BR_DEFAULT)
#undef BR_DEFAULT
        const unsigned char colors[BR_COLOR_COUNT][3] = {
            {43,124,255}, {255,86,86}, {120,255,140}, {86,255,246},
            {158,158,158}, {255,255,255}, {255,255,255}, {255,255,255},
            {255,255,255}, {255,255,255}, {255,255,255}, {120,255,140},
            {255,176,66}, {139,92,246}, {220,220,220},
            {255,255,255}, {255,255,255}, {177,239,255}, {51,179,255},
            {10,77,184}, {255,86,86}, {90,170,255}, {120,255,140},
            {90,170,255}, {190,110,255}, {255,0,200},
            {255,140,60}, {255,90,90}, {110,155,205}, {152,165,180},
            {255,0,200}, {43,124,255}, {255,255,255}, {255,255,255},
            {255,255,255}, {255,255,255}, {255,255,255},
            {255,0,200}, {255,0,200}, {90,170,255},
        };
        for (int i = 0; i < BR_COLOR_COUNT; ++i)
            for (int k = 0; k < 3; ++k) defaults.color[i].rgb[k] = colors[i][k];
        return defaults;
    }();
    return &config;
}

int br_config_set(BR_Config* config, const char* key, int value) {
    (void)config; (void)key; (void)value;
    return 0;
}
int br_config_save(void) { return 0; }
int br_config_load(void) { return 0; }

/* The title clips the OS cursor to a 1x1 rect while it has mouse-look. ImGui
 * reads the real cursor, so the menu is unmovable until that clip is lifted;
 * Unity only reapplies it on a lock-state change, so releasing on open and
 * restoring the captured rect on close is enough. */
static RECT g_saved_clip;
static volatile LONG g_clip_saved;

static void release_cursor(void) {
    if (InterlockedExchange(&g_clip_saved, 1) == 0) {
        memset(&g_saved_clip, 0, sizeof(g_saved_clip));
        GetClipCursor(&g_saved_clip);
    }
    ClipCursor(NULL);
    ShowCursor(TRUE);
}

static void restore_cursor(void) {
    /* Nothing was released (the game's own CursorLockModel handled the menu),
     * so there is no saved rect to put back. */
    if (!InterlockedCompareExchange(&g_clip_saved, 0, 0)) return;
    ClipCursor(&g_saved_clip);
    ShowCursor(FALSE);
    LONG cx = (g_saved_clip.left + g_saved_clip.right) / 2;
    LONG cy = (g_saved_clip.top + g_saved_clip.bottom) / 2;
    SetCursorPos(cx, cy);
}

void br_set_menu_visible(int visible) {
    const int was = InterlockedCompareExchange(&g_menu_visible, 0, 0);
    br_get()->menu_open = visible != 0;
    InterlockedExchange(&g_menu_visible, visible != 0);
    amphetaminehaxx::set_visible(visible != 0);
    /* The game owns mouse-look through CursorLockModel. Its SideMenu lock is
     * what actually frees the cursor and stops clicks reaching the world; the
     * Win32 clip release stays as a fallback when the model is not resolved
     * yet (lobby, loading, wrong build). */
    const int game_cursor = game_cursor_menu(visible != 0);
    if (visible && !was && !game_cursor) release_cursor();
    else if (!visible && was && !game_cursor) restore_cursor();
}
void br_request_toggle(void) { br_set_menu_visible(!InterlockedCompareExchange(&g_menu_visible, 0, 0)); }
int br_menu_hotkey(void) { return BR_DEFAULT_MENU_KEY; }
void br_set_menu_hotkey(int key) { (void)key; }
int br_start_menu(void) { br_set_menu_visible(1); return 0; }
void br_stop_menu(void) { br_set_menu_visible(0); }

/* ---- game data thread ---- */

static CRITICAL_SECTION g_log_lock;
static char g_log_path[MAX_PATH];
static char* append_text(char* p, const char* s);

static char* append_digits(char* p, unsigned value, unsigned width) {
    char* end = p + width;
    for (char* digit = end; digit != p; value /= 10) *--digit = (char)('0' + value % 10);
    return end;
}

static char* log_prefix(char* p, const char* channel) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    p = append_text(p, "< Amphetamine > [");
    p = append_digits(p, now.wMonth, 2); *p++ = '/';
    p = append_digits(p, now.wDay, 2); *p++ = '/';
    p = append_digits(p, now.wYear, 4); *p++ = ' ';
    p = append_digits(p, now.wHour, 2); *p++ = ':';
    p = append_digits(p, now.wMinute, 2); *p++ = ':';
    p = append_digits(p, now.wSecond, 2);
    p = append_text(p, "] ");
    return append_text(append_text(p, channel), " > ");
}

/* The host runs from Program Files, so a bare relative name lands in a
 * directory the mapped image cannot write to; keep the log under
 * %LOCALAPPDATA% instead. */
static const char* log_path(void) {
    if (!g_log_path[0]) {
        char dir[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, (DWORD)sizeof(dir));
        if (n > 0 && n < sizeof(dir)) {
            strcat_s(dir, sizeof(dir), "\\RobRunInternal");
            CreateDirectoryA(dir, NULL);
            _snprintf_s(g_log_path, sizeof(g_log_path), _TRUNCATE, "%s\\rr_internal.log", dir);
        } else {
            strcpy_s(g_log_path, sizeof(g_log_path), "rr_internal.log");
        }
    }
    return g_log_path;
}

static void internal_log(const char* text) {
    EnterCriticalSection(&g_log_lock);
    FILE* f = fopen(log_path(), "a");
    if (f) {
        char prefix[64];
        *log_prefix(prefix, "overlay") = 0;
        fprintf(f, "%s%s\n", prefix, text);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

static void publish_log_accent(void) {
    static unsigned published = ~0u;
    const unsigned accent = br_get()->hud_accent_rgb & 0xFFFFFFu;
    if (accent == published) return;
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, (DWORD)sizeof(path));
    const char suffix[] = "\\RobRunInternal\\accent.txt";
    if (!n || n >= sizeof(path) - sizeof(suffix)) return;
    memcpy(path + n, suffix, sizeof(suffix));
    FILE* f = fopen(path, "w");
    if (!f) return;
    const bool written = fprintf(f, "%06X\n", accent) > 0;
    if (fclose(f) == 0 && written) published = accent;
}

static void internal_logf(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    internal_log(line);
}

extern "C" void game_status(char* out, size_t n);

/* Shared sink for the C modules (chams) so their lines land in the same log. */
extern "C" void rr_int_log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    internal_log(line);
}


/* ---- crash forensics ---- */

/* The last-chance handler runs with a possibly poisoned CRT heap, so this path
 * uses CreateFileA/WriteFile only: no stdio, no locks. A missing line in the
 * log after a death is itself evidence: it means the process was terminated
 * from outside (EAC) instead of faulting inside this image. */
static void crash_log_line(const char* text) {
    HANDLE f = CreateFileA(log_path(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD len = 0, wrote = 0;
    while (text[len]) ++len;
    WriteFile(f, text, len, &wrote, NULL);
    WriteFile(f, "\r\n", 2, &wrote, NULL);
    CloseHandle(f);
}

static void module_of(const void* address, char* out, size_t n, uint64_t* rva) {
    *rva = 0;
    HMODULE mod = NULL;
    if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      (LPCSTR)address, &mod) && mod) {
        char path[MAX_PATH] = { 0 };
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = path;
        for (const char* p = path; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        snprintf(out, n, "%s", base[0] ? base : "?");
        *rva = (uint64_t)(uintptr_t)address - (uint64_t)(uintptr_t)mod;
        return;
    }
    /* The mapped image has no loader entry; find its PE base by hand. */
    HMODULE self = address ? image_base_of(address) : NULL;
    if (self) {
        snprintf(out, n, "RobRunInternal.dll");
        *rva = (uint64_t)(uintptr_t)address - (uint64_t)(uintptr_t)self;
        return;
    }
    snprintf(out, n, "?");
}

static char* appendf(char* p, char* end, const char* fmt, ...) {
    if (p >= end) return end;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(p, (size_t)(end - p), fmt, ap);
    va_end(ap);
    if (n < 0) { *p = 0; return end; }
    p += n;
    return p > end ? end : p;
}

/* Resolve the return addresses sitting on the faulting thread's stack. The
 * mapped image is attributed by image_base_of, so a frame inside our own code
 * reads as RobRunInternal.dll+rva and a frame that resolves to nothing at all
 * is a wild call - the "?+0x0" line had no way to tell those apart. */
static void append_stack(char* p, char* end, const CONTEXT* ctx) {
    if (!ctx) return;
    p = appendf(p, end, " bt=");
    const uint64_t rsp = (uint64_t)ctx->Rsp;
    int shown = 0;
    for (int i = 0; i < 512 && shown < 10; ++i) {
        uint64_t slot = 0;
        __try {
            slot = *(const uint64_t*)(uintptr_t)(rsp + (uint64_t)i * 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        char name[64] = { 0 };
        uint64_t rva = 0;
        module_of((const void*)(uintptr_t)slot, name, sizeof(name), &rva);
        if (!name[0] || name[0] == '?') continue;
        p = appendf(p, end, " %s+0x%llx", name, (unsigned long long)rva);
        ++shown;
    }
}

static void crash_detail(char* line, size_t cap, const char* tag, EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info ? info->ExceptionRecord : NULL;
    const CONTEXT* ctx = info ? info->ContextRecord : NULL;
    char module[MAX_PATH] = { 0 };
    uint64_t rva = 0;
    module_of(rec ? rec->ExceptionAddress : NULL, module, sizeof(module), &rva);
    char* p = line;
    char* end = line + cap - 1;
    p = appendf(p, end,
                "%s code=0x%lx addr=%s+0x%llx ex=%p phase=%d rphase=%ld players=%d presents=%lu tids=%lu",
                tag, rec ? (unsigned long)rec->ExceptionCode : 0ul, module,
                (unsigned long long)rva, rec ? rec->ExceptionAddress : NULL,
                game_phase_get(), (long)InterlockedCompareExchange(&g_rphase, 0, 0),
                g_last_players, g_present_n, (unsigned long)GetCurrentThreadId());
    if (rec && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR op = rec->ExceptionInformation[0];
        p = appendf(p, end, " access=%s data=%p",
                    op == 1 ? "write" : op == 8 ? "execute" : "read",
                    (void*)(uintptr_t)rec->ExceptionInformation[1]);
    }
    if (ctx)
        p = appendf(p, end, " rip=%p rsp=%p rbp=%p",
                    (void*)(uintptr_t)ctx->Rip, (void*)(uintptr_t)ctx->Rsp,
                    (void*)(uintptr_t)ctx->Rbp);
    if (p < end) { *p = 0; append_stack(p, end, ctx); }
    *end = 0;
}

/* First chance: only the case the last-chance handler cannot describe - a fault
 * whose instruction pointer belongs to no module and no mapped image. Those are
 * jumps through a freed or corrupted pointer, and by the time the filter runs
 * the stack that explains them is gone. */
static LONG CALLBACK first_chance_filter(EXCEPTION_POINTERS* info) {
    static volatile LONG busy;
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        rec->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        rec->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    char module[MAX_PATH] = { 0 };
    uint64_t rva = 0;
    module_of(rec->ExceptionAddress, module, sizeof(module), &rva);
    if (module[0] && module[0] != '?') return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&busy, 1)) return EXCEPTION_CONTINUE_SEARCH;
    char line[1600];
    crash_detail(line, sizeof(line), "WILD", info);
    crash_log_line(line);
    InterlockedExchange(&busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_EXECUTE_HANDLER;
    char line[1600];
    crash_detail(line, sizeof(line), "CRASH", info);
    crash_log_line(line);
    return EXCEPTION_EXECUTE_HANDLER;
}


static DWORD WINAPI game_worker(LPVOID param) {
    (void)param;
    int ok = 0;
    /* The roster only exists once the title has a live world, so keep trying
     * across lobby/loading transitions instead of giving up after one scan. */
    for (int attempt = 0; !game_discovery_active(); attempt++) {
        if (attempt) Sleep(500);
        const DWORD started = GetTickCount();
        ok = game_init();
        if (!ok) {
            char status[256] = { 0 };
            game_status(status, sizeof(status));
            snprintf(br_get()->telem_status, sizeof(br_get()->telem_status), "%s", status);
            internal_logf("init attempt %d (%lu ms): %s roster=%d world=%llx", attempt + 1,
                          GetTickCount() - started, status, game_roster_ready(),
                          (unsigned long long)game_world_addr());
        } else {
            internal_logf("init attempt %d ok in %lu ms", attempt + 1, GetTickCount() - started);
        }
    }
    /* A failed init leaves world/class handles unset; letting the draw path
     * run against them was crashing the title once players appeared. */
    if (ok) internal_log("game_init ok");
    auto* staging = (RR_PLAYER*)VirtualAlloc(NULL, sizeof(RR_PLAYER) * snapshot_capacity * 2,
                                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!staging) return 0;
    auto* published = staging + snapshot_capacity;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_MODIFY_STATE | SYNCHRONIZE);
    constexpr DWORD sample_period_ms = 8;
    for (;;) {
        const ULONGLONG started = GetTickCount64();
        {
            const unsigned generation = game_world_generation();
            InterlockedExchange(&g_ready, game_roster_ready() ? 1 : 2);
            RR_CAMERA camera{};
            const int count = game_snapshot(staging, snapshot_capacity, &camera);
            AcquireSRWLockExclusive(&g_snapshot_lock);
            RR_PLAYER* old = published;
            published = staging;
            staging = old;
            g_snapshot_players = published;
            g_snapshot_camera = camera;
            g_snapshot_count = count;
            g_snapshot_generation = generation;
            g_snapshot_at = GetTickCount();
            ++g_snapshot_sequence;
            ReleaseSRWLockExclusive(&g_snapshot_lock);
        }
        const ULONGLONG elapsed = GetTickCount64() - started;
        const DWORD delay = elapsed < sample_period_ms ? sample_period_ms - (DWORD)elapsed : 1;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)delay * 10000;
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) ||
            WaitForSingleObject(timer, delay + 20) != WAIT_OBJECT_0)
            Sleep(delay);
    }
    return 0;
}

/* ---- renderer bootstrap ---- */

/* Matches the bake size rr_draw.cpp draws ESP labels at. The face is compiled
 * into .rdata instead of a .rsrc entry: a manually mapped image does not join
 * the loader list and its PE headers may be gone, which is why the resource
 * path silently fell back to the ugly default bitmap face. */
/* Whole pixels only. A fractional bake (12.72) leaves the glyph raster at one
 * size and every label quad at another, so the GPU resamples the atlas and the
 * strokes read as both jagged and blurred. Baked at the draw size. */
static constexpr float kEspFontPx = 13.0f;

static bool init_render(IDXGISwapChain* swap, const DXGI_SWAP_CHAIN_DESC& desc) {
    ComPtr<ID3D11Device> device;
    if (FAILED(swap->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        internal_log("GetDevice failed");
        return false;
    }
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return false;

    if (!ImGui::GetCurrentContext()) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = NULL;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        char* slash = strrchr(exe, '\\');
        if (slash) *slash = 0;
        amphetaminehaxx::initialize(std::string(exe) + "\\daydream-data");
        gpu_font(15.f);
        if (menu::dist_font) gpu_set_esp_font(menu::dist_font, kEspFontPx);
        amphetaminehaxx::set_visible(false);
    }
    if (g_device != device.Get() || g_window != desc.OutputWindow) {
        if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX11_Shutdown();
        if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
        if (!ImGui_ImplWin32_Init(desc.OutputWindow) || !ImGui_ImplDX11_Init(device.Get(), context.Get())) {
            g_swap = NULL;
            internal_log("ImGui backend init failed");
            return false;
        }
        gpu_set_device(device.Get());
        if (g_context) g_context->Release();
        if (g_device) g_device->Release();
        g_context = context.Detach();
        g_device = device.Detach();
        g_window = desc.OutputWindow;
    }
    static bool worker_started;
    if (!worker_started) {
        HANDLE worker = rr_thread_start(game_worker, NULL);
        if (!worker) return false;
        CloseHandle(worker);
        worker_started = true;
        rr_los_start();
    }
    g_is_flip = desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    g_buffer_count = (int)desc.BufferCount;
    g_swap = swap;
    internal_logf("renderer bound to swap=%p hwnd=%p", (void*)swap, (void*)g_window);
    return true;
}

static void feed_mouse() {
    ImGuiIO& io = ImGui::GetIO();
    /* ImGui_ImplWin32 only publishes the cursor when the backend window owns
     * focus; drive the position explicitly so the menu tracks the real cursor. */
    if (g_window) {
        POINT p;
        if (GetCursorPos(&p) && ScreenToClient(g_window, &p))
            io.AddMousePosEvent((float)p.x, (float)p.y);
    }
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
}

/* ---- aim assist ---- */

static RR_AIM_STEER g_steer;
static uint64_t g_aim_locked_addr;
static int32_t g_aim_locked_id;
static RR_V3 g_aim_last_pos;
static unsigned long g_aim_last_at, g_aim_last_step;
static volatile LONG g_aim_toggle, g_aim_key_prev;

/* aa_bind_mode: 0 hold, 1 toggle, 2 always. */
static void aim_key_poll(const BR_Config* b) {
    if (b->aa_key <= 0 || b->aa_key > 255 || b->aa_bind_mode != 1) {
        InterlockedExchange(&g_aim_toggle, 0);
        InterlockedExchange(&g_aim_key_prev, 0);
        return;
    }
    const LONG down = (GetAsyncKeyState(b->aa_key) & 0x8000) ? 1 : 0;
    const LONG was = InterlockedExchange(&g_aim_key_prev, down);
    if (down && !was) InterlockedXor(&g_aim_toggle, 1);
}

static int aim_key_held(const BR_Config* b) {
    if (b->aa_key <= 0 || b->aa_key > 255 || b->aa_bind_mode == 2) return 1;
    if (b->aa_bind_mode == 1) return InterlockedCompareExchange(&g_aim_toggle, 0, 0) != 0;
    return (GetAsyncKeyState(b->aa_key) & 0x8000) != 0;
}

static void aim_move(int dx, int dy) {
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(input));
}

static void aim_release(void) {
    game_cam_clear();
    g_aim_locked_addr = 0;
    g_aim_locked_id = 0;
    g_steer = RR_AIM_STEER{};
    g_aim_last_at = 0;
}

static void aim_tick(const RR_PLAYER* players, int count, const RR_CAMERA* cam) {
    BR_Config* b = br_get();
    aim_key_poll(b);
    const int menu_open = InterlockedCompareExchange(&g_menu_visible, 0, 0) != 0;
    if (!b->aa_on || menu_open || GetForegroundWindow() != g_window || !aim_key_held(b)) {
        aim_release();
        return;
    }
    const bool camera_lock = b->aa_mode == 2;
    static bool was_camera_lock;
    if (camera_lock != was_camera_lock) {
        aim_release();
        was_camera_lock = camera_lock;
    }
    if (!cam->valid || count <= 0) {
        if (!camera_lock) aim_release();
        return;
    }
    /* Mouse mode needs time for the game to consume the previous input. */
    const unsigned long now = GetTickCount();
    if (!camera_lock && g_aim_last_step && now - g_aim_last_step < 15) return;
    const unsigned elapsed = g_aim_last_step ? (unsigned)(now - g_aim_last_step) : 16u;
    g_aim_last_step = now;

    const int vw = (int)g_width, vh = (int)g_height;
    /* Sticky aim hands the picker the current lock, so a new player walking
     * into the capture ring cannot take the target mid-bind. Releasing the bind
     * runs aim_release(), which drops the lock and lets the next press acquire
     * whoever is under the reticle. */
    const bool sticky = camera_lock || b->aa_sticky;
    const uint64_t lock_addr = sticky ? g_aim_locked_addr : 0;
    const int32_t lock_id = sticky ? g_aim_locked_id : 0;
    BR_Config selection = *b;
    if (camera_lock) selection.aa_bone = RR_AIM_HEAD;
    RR_AIM_PICK pick = rr_aim_pick(players, count, cam, vw, vh, &selection, lock_addr, lock_id, NULL);
    if (!pick.player) {
        if (!camera_lock || !g_aim_last_at || now - g_aim_last_at > 100) aim_release();
        return;
    }
    const RR_PLAYER* p = pick.player;
    if (camera_lock) {
        if (!game_cam_track(cam, p->addr, pick.point, b->aa_smooth)) return;
        g_aim_locked_addr = p->addr;
        g_aim_locked_id = p->id;
        g_aim_last_at = now;
        return;
    }
    game_cam_clear();
    float dx = pick.dx, dy = pick.dy;
    if (g_aim_last_at && !p->inveh && p->addr == g_aim_locked_addr && p->id == g_aim_locked_id) {
        RR_V3 predicted;
        float sx, sy;
        if (rr_aim_motion_point(pick.point, p->pos, g_aim_last_pos, now - g_aim_last_at, 0,
                                b->aa_smooth, &predicted) &&
            game_w2s(cam, &predicted, vw, vh, &sx, &sy)) {
            dx = sx - vw * 0.5f;
            dy = sy - vh * 0.5f;
        }
    }
    g_aim_locked_addr = p->addr;
    g_aim_locked_id = p->id;
    g_aim_last_pos = p->pos;
    g_aim_last_at = now;

    const int firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    int mx = 0, my = 0;
    rr_aim_step(&g_steer, dx, dy, cam->fov, vh, b->aa_smooth, b->aa_aim_cal, elapsed, firing,
                &mx, &my);
    if (mx || my) aim_move(mx, my);
}

static UINT back_buffer_index(IDXGISwapChain* swap) {    if (!InterlockedCompareExchange(&g_is_flip, 0, 0)) return 0;
    if (g_chain3_owner != swap) {
        if (g_chain3) g_chain3->Release();
        g_chain3 = NULL;
        swap->QueryInterface(IID_PPV_ARGS(&g_chain3));
        /* Cache the failure too - a legacy chain would retry every frame. */
        g_chain3_owner = swap;
    }
    if (!g_chain3) return 0;
    const UINT index = g_chain3->GetCurrentBackBufferIndex();
    return index < (UINT)g_buffer_count ? index : 0;
}

static void draw_frame(IDXGISwapChain* swap) {
    static DWORD last_snapshot;
    static uint64_t last_sequence;
    static unsigned last_generation;
    static int full_rigs, partial_rigs;
    if (g_present_n == 1) internal_log("first frame on the title's swap chain");
    const UINT index = back_buffer_index(swap);
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swap->GetBuffer(index, IID_PPV_ARGS(&backbuffer))) || !backbuffer) {
        ++g_buf_fail;
        return;
    }
    D3D11_TEXTURE2D_DESC buffer_desc;
    backbuffer->GetDesc(&buffer_desc);
    g_width = buffer_desc.Width;
    g_height = buffer_desc.Height;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    feed_mouse();

    if (GetAsyncKeyState(br_menu_hotkey()) & 1) br_request_toggle();

    ImGui::NewFrame();
    amphetaminehaxx::render();
    static ULONGLONG next_accent_at;
    const ULONGLONG accent_now = GetTickCount64();
    if (accent_now >= next_accent_at) {
        next_accent_at = accent_now + 1000;
        publish_log_accent();
    }

    {
        const unsigned long now = GetTickCount();
        /* The menu mirrors the scene worker's live verdicts, not the first
         * init attempt: a stale line read as "dead" while targets were fine. */
        static unsigned long last_status;
        if (now - last_status >= 1000) {
            last_status = now;
            char status[256] = { 0 };
            game_status(status, sizeof(status));
            BR_Config* cfg = br_get();
            snprintf(cfg->telem_status, sizeof(cfg->telem_status), "%s", status);
            cfg->telem_game_pid = (int)GetCurrentProcessId();
        }
        /* Keep a render-local copy so drawing never holds the producer lock. */
        static RR_PLAYER* players;
        static RR_CAMERA camera;
        if (!players) {
            players = (RR_PLAYER*)VirtualAlloc(NULL, sizeof(RR_PLAYER) * snapshot_capacity,
                                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!players) {
                ImGui::EndFrame();
                return;
            }
        }
        if (TryAcquireSRWLockShared(&g_snapshot_lock)) {
            if (g_snapshot_players && last_sequence != g_snapshot_sequence) {
                last_sequence = g_snapshot_sequence;
                last_snapshot = g_snapshot_at;
                last_generation = g_snapshot_generation;
                g_last_players = g_snapshot_count;
                camera = g_snapshot_camera;
                memcpy(players, g_snapshot_players, sizeof(RR_PLAYER) * g_last_players);
                full_rigs = partial_rigs = 0;
                for (int i = 0; i < g_last_players; ++i) {
                    if (players[i].full_rig) ++full_rigs;
                    else if (players[i].nbones > 0) ++partial_rigs;
                }
            }
            ReleaseSRWLockShared(&g_snapshot_lock);
        }
        const int count = last_sequence && last_generation == game_world_generation() &&
                          GetTickCount() - last_snapshot <= 250 ? g_last_players : 0;
        RR_CAMERA render_camera = camera;
        if (count) game_refresh_render_camera(&render_camera);
        if (count) {
            game_cam_render_state(&render_camera);
            game_refresh_render_players(players, count, &render_camera);
        }
        /* FOV pin stays live with the menu closed or the aim bind idle. */
        game_cam_fov((float)br_get()->cam_fov);
        InterlockedExchange(&g_rphase, 2);
        aim_tick(players, count, &render_camera);
        /* Draw every present, roster or not: the aim ring and the self line are
         * roster-independent and their absence in the lobby read as a dead
         * overlay. */
        InterlockedExchange(&g_rphase, 3);
        draw_esp(players, count, &render_camera,
                 (int)g_width, (int)g_height);
        if (!count) ++g_empty_n;
    }
    ImGui::Render();

    InterlockedExchange(&g_rphase, 4);

    {
        ComPtr<ID3D11RenderTargetView> target;
        if (FAILED(g_device->CreateRenderTargetView(backbuffer.Get(), NULL, &target))) {
            ++g_rtv_fail;
        } else {
            InterlockedExchange(&g_rphase, 5);
            g_context->OMSetRenderTargets(1, target.GetAddressOf(), NULL);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_context->OMSetRenderTargets(0, NULL, NULL);
            ++g_draw_n;
        }
    }
    InterlockedExchange(&g_rphase, 0);

    if ((g_present_n % 120) == 0)
        internal_logf("present #%lu draws=%lu empty=%lu buf_fail=%lu rtv_fail=%lu test=%lu idx=%u buffers=%d ready=%d players=%d full_rigs=%d partial_rigs=%d phase=%d",
                      g_present_n, g_draw_n, g_empty_n, g_buf_fail, g_rtv_fail, g_test_skip,
                      index, g_buffer_count, (int)InterlockedCompareExchange(&g_ready, 0, 0),
                      g_last_players, full_rigs, partial_rigs, game_phase_get());
}

static void on_present(IDXGISwapChain* swap, UINT sync, UINT flags, const char* slot) {
    if (flags & DXGI_PRESENT_TEST) { ++g_test_skip; return; }
    static SRWLOCK render_lock = SRWLOCK_INIT;
    if (!TryAcquireSRWLockExclusive(&render_lock)) return;
    if (swap != g_swap) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (FAILED(swap->GetDesc(&desc)) || !desc.OutputWindow ||
            (g_window && g_window != desc.OutputWindow && IsWindow(g_window)) ||
            !init_render(swap, desc)) {
            ReleaseSRWLockExclusive(&render_lock);
            return;
        }
    }
    ++g_present_n;
    if (g_present_n <= 8)
        internal_logf("%s #%lu sync=%u flags=0x%x", slot, g_present_n, sync, flags);
    draw_frame(swap);
    ReleaseSRWLockExclusive(&render_lock);
}

static HRESULT WINAPI present_hook(IDXGISwapChain* swap, UINT sync, UINT flags) {
    on_present(swap, sync, flags, "present");
    return g_present_original(swap, sync, flags);
}

static HRESULT WINAPI present1_hook(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    (void)params;
    on_present(swap, sync, flags, "present1");
    /* Forward with no dirty rects: a partial copy of the frame would drop the
     * ESP drawn outside the game's own update region. */
    return g_present1_original(swap, sync, flags, NULL);
}

/* Patch the Present slot of the dxgi swap-chain vtable. Every swap chain the
 * process creates shares that vtable, so the game's own chain is covered
 * without touching the title or waiting for a frame. */
static int install_hook(void) {
    struct Dummy { HWND window; } dummy = { NULL };

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RRInternalProbe";
    RegisterClassExW(&wc);
    dummy.window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
                                   0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    if (!dummy.window) {
        internal_logf("probe window failed (%lu)", GetLastError());
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    DXGI_SWAP_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = dummy.window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* probe_swap = NULL;
    ID3D11Device* probe_device = NULL;
    ID3D11DeviceContext* probe_context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
        &desc, &probe_swap, &probe_device, &level, &probe_context);

    int ok = 0;
    if (SUCCEEDED(hr) && probe_swap) {
        void** vtable = *(void***)probe_swap;
        g_present_original = (PRESENT_FN)vtable[8];
        g_present1_original = (PRESENT1_FN)vtable[22];
        DWORD old = 0;
        /* Slots 8..22 hold Present..Present1; one region covers both. */
        if (VirtualProtect(&vtable[8], sizeof(void*) * 15, PAGE_READWRITE, &old)) {
            vtable[8] = (void*)present_hook;
            vtable[22] = (void*)present1_hook;
            VirtualProtect(&vtable[8], sizeof(void*) * 15, old, &old);
            ok = 1;
        } else {
            internal_logf("probe VirtualProtect failed (%lu)", GetLastError());
        }
    } else {
        internal_logf("probe device failed hr=0x%08lX swap=%p", (unsigned long)hr, (void*)probe_swap);
    }
    if (probe_context) probe_context->Release();
    if (probe_device) probe_device->Release();
    if (probe_swap) probe_swap->Release();
    if (dummy.window) DestroyWindow(dummy.window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

static void boot_log(const char* text);

/* DllMain runs under the loader lock. D3D11CreateDeviceAndSwapChain pulls
 * dxgi/d3d11 in through that same loader, so a probe started here either
 * deadlocks or fails outright; the install is handed to a worker instead.
 * The worker's start address is inside this module, so the thread is not
 * anonymous to a checker that validates thread entry points. */
static DWORD WINAPI hook_installer(LPVOID param) {
    (void)param;
    for (int attempt = 0; attempt < 480; attempt++) {
        if (install_hook()) {
            InterlockedExchange(&g_hook_installed, 1);
            internal_logf("Present hook installed on attempt %d", attempt);
            return 0;
        }
        Sleep(250);
    }
    internal_log("Present hook could not be installed");
    return 0;
}

static DWORD WINAPI bootstrap(LPVOID param) {
    (void)param;
    boot_log("bootstrap running");
    /* Armed before the hook so a fault in the first frames still leaves a line
     * naming the module, the phase and the roster state. */
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(1, first_chance_filter);
    internal_log("crash handler armed");
    /* Diagnostic gate: RR_NO_HOOK=1 keeps the module resident without touching
     * the swap-chain vtable, which separates module-presence checks from
     * hook-integrity checks. */
    char gate[8];
    if (GetEnvironmentVariableA("RR_NO_HOOK", gate, (DWORD)sizeof(gate))) {
        internal_log("hook disabled by RR_NO_HOOK");
        return 0;
    }
    HANDLE worker = CreateThread(NULL, 0, hook_installer, NULL, 0, NULL);
    if (worker) CloseHandle(worker);
    else internal_logf("hook worker creation failed (%lu)", GetLastError());
    return 0;
}

/* ---- module attach ---- */

/* Manual maps have no loader entry, so the image base is found by walking
 * back to the PE headers. VirtualQuery gates every probe, which keeps this
 * safe to run before the unwind table is registered. */
static HMODULE image_base_of(const void* address) {
    const BYTE* p = (const BYTE*)address;
    for (size_t back = 0; back <= 0x400000; back += 0x1000) {
        const BYTE* cand = p - back;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(cand, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
        if (cand + sizeof(IMAGE_DOS_HEADER) > (const BYTE*)mbi.BaseAddress + mbi.RegionSize) continue;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)cand;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            dos->e_lfanew > 0x1000) continue;
        if (cand + dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) >
            (const BYTE*)mbi.BaseAddress + mbi.RegionSize) continue;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(cand + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) continue;
        /* The address has to fall inside the image. Without this a heap block
         * whose first two bytes happen to be "MZ" steals the attribution and a
         * fault in the mapped image is reported as "?" forever. */
        if ((const BYTE*)address >= cand + nt->OptionalHeader.SizeOfImage) continue;
        return (HMODULE)(uintptr_t)cand;
    }
    return NULL;
}

static void attach_module(HMODULE self) {
    InitializeCriticalSection(&g_log_lock);
    DisableThreadLibraryCalls(self);
    bootstrap(NULL);
}

/* A mapped image never gets its .pdata registered, so __try/__except and C++
 * unwinding would both terminate the process instead of walking out. */
static void register_unwind(HMODULE self) {
    if (!self) return;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)self;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)((const BYTE*)self + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dir->VirtualAddress || dir->Size < 12) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    typedef BOOLEAN (NTAPI* FN_RAFT)(PRUNTIME_FUNCTION, DWORD, ULONGLONG);
    FN_RAFT add = ntdll ? (FN_RAFT)(void*)GetProcAddress(ntdll, "RtlAddFunctionTable") : NULL;
    if (add)
        add((PRUNTIME_FUNCTION)((const BYTE*)self + dir->VirtualAddress), dir->Size / 12,
            (ULONGLONG)(uintptr_t)self);
}

extern "C" BOOL __stdcall _CRT_INIT(void* hinst, unsigned long reason, void* reserved);
extern "C" __declspec(dllexport) BOOL rr_mapped_entry(HINSTANCE inst, DWORD reason, LPVOID reserved);

/* Raw Win32 logging for the entry path: it runs before _CRT_INIT, so the
 * static CRT's formatting helpers are off limits. Everything here is a
 * plain loop over bytes. */
static char* append_text(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static char* append_hex(char* p, unsigned long long v) {
    *p++ = '0'; *p++ = 'x';
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const unsigned digit = (unsigned)((v >> shift) & 0xF);
        if (!digit && !started && shift) continue;
        started = true;
        *p++ = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
    }
    return p;
}
static char* append_dec(char* p, unsigned long v) {
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static void boot_log(const char* text) {
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, (DWORD)sizeof(path));
    if (!n || n >= sizeof(path)) return;
    char* p = path + n;
    const char* dir = "\\RobRunInternal";
    if (n + 15 + 11 >= (DWORD)sizeof(path)) return;
    p = append_text(p, dir);
    *p = 0;
    CreateDirectoryA(path, NULL);
    p = append_text(p, "\\entry.log");
    *p = 0;
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char line[256];
    char* end = append_text(log_prefix(line, "entry"), text);
    end = append_text(end, "\r\n");
    DWORD wrote = 0;
    WriteFile(f, line, (DWORD)(end - line), &wrote, NULL);
    CloseHandle(f);
}

/* Export name for loaders that call a symbol instead of the PE entry point
 * (the orelia injector looks for "DllMain"). */
extern "C" __declspec(dllexport) BOOL rr_mapped_entry(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    {
        char line[160];
        char* p = append_text(line, "entry rcx=");
        p = append_hex(p, (unsigned long long)(uintptr_t)inst);
        p = append_text(p, " reason=");
        p = append_dec(p, reason);
        p = append_text(p, " thread=");
        p = append_dec(p, GetCurrentThreadId());
        *p = 0;
        boot_log(line);
    }
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    /* The orelia shellcode passes the mapped image base in r8 (reserved);
     * only fall back to the backward scan when the caller leaves it empty. */
    HMODULE self = (HMODULE)(uintptr_t)reserved;
    if (!self || ((const IMAGE_DOS_HEADER*)self)->e_magic != IMAGE_DOS_SIGNATURE)
        self = image_base_of((const void*)&image_base_of);
    {
        char line[64];
        char* p = append_text(line, "image base ");
        p = append_hex(p, (unsigned long long)(uintptr_t)self);
        *p = 0;
        boot_log(line);
    }
    register_unwind(self);
    boot_log("unwind registered");
    _CRT_INIT((void*)self, DLL_PROCESS_ATTACH, reserved);
    boot_log("crt init done");
    attach_module(self);
    boot_log("worker created");
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) attach_module(instance);
    return TRUE;
}
