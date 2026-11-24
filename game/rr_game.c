/* rr_game.c - Discover live ROB RUN objects and build render snapshots.
 *
 * The preferred roster comes from the active world dictionary. Bounded
 * instance scans provide a fallback when anchors cannot be resolved. A
 * background worker refreshes the roster and world caches; the sampling path
 * copies published entries under a lock, then reads transforms and rigs.
 *
 * Managed field offsets come from the generated headers. Native transforms,
 * generic wrappers and camera layouts use separately checked assumptions.
 * All cached addresses must be validated after scene or process changes.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>

#include "memory/rr_mem.h"
#include "rr_unity.h"
#include "memory/rr_chain.h"
#include "ui/menu_bridge.h"
#include "rr_game.h"
#include "rr_threads.h"
#include "game/offsets/robrun_offsets.h"
#include "game/offsets/robrun_sdk_offsets.h"
#include "game/rig/rr_cham_model.h"
#include "game/rig/rr_rig_map.h"
#include "game/rig/rr_pose_gate.h"
#include "game/rig/rr_cham_coherence.h"
#include "features/esp/rr_world_marker_select.h"
#include "rr_hiding.h"
#ifdef RR_INTERNAL
#include "features/esp/rr_draw.h"
#include "features/visibility/rr_los.h"
#endif

/* These fields are read as packed XYZ triples below. A new SDK layout must
 * fail compilation rather than silently keep using a twelve-byte read. */
#if RR_OFF_POS_Y != RR_OFF_POS_X + 4 || RR_OFF_POS_Z != RR_OFF_POS_X + 8 || \
    RR_OFF_INTERACT_POS_Y != RR_OFF_INTERACT_POS_X + 4 || \
    RR_OFF_INTERACT_POS_Z != RR_OFF_INTERACT_POS_X + 8
#error Position fields are no longer contiguous; update the position reader.
#endif

#define RUNTIME_FILE "robrun_runtime.txt"

/* Generic-wrapper offsets (measured, not in the metadata):
 *   PrimitiveData<T>.value +0x18, ReferenceData<T>.value +0x18,
 *   EnumItem.Name string +0x10. */
#define RR_OFF_PRIM_VALUE 0x18
#define RR_OFF_REF_VALUE  0x18
#define RR_OFF_DESC_NAME  0x10
#define TR_READ_LEN  (RR_OFF_CTM_ROLL + 4 - RR_OFF_CTM_X)   /* X..Roll = 24 B */
#define XF_THREADS 8
#define BATCH_MAX 64
static void xf_pool_init(void);
#define MAX_HITS 64
static uint32_t g_saved_pid = 0;
static uint64_t g_saved_world = 0;
static uint64_t g_k_world;
static uint64_t g_world;
static uint64_t g_battle = 0;
static uint64_t g_battle_model = 0;
static uint64_t g_views_updater = 0;
static volatile LONG64 g_time_model;
static volatile LONG64 g_time_world;
static volatile LONG g_tick_period;
static int g_pose_clock_changed;
static int g_pose_diag_enabled;
static RR_POSE_DIAG g_pose_diag;
void game_pose_diag_enable(void) { memset(&g_pose_diag,0,sizeof(g_pose_diag)); g_pose_diag_enabled=1; }
RR_POSE_DIAG game_pose_diag_results(void) { return g_pose_diag; }
static uint64_t g_aim_updater = 0;
#define MAX_CANDS 16
static uint64_t g_hits[MAX_HITS];
static volatile LONG g_nhits;
static uint64_t g_cands[MAX_CANDS];
static int g_ncands;
static void collect_hits(const char* name);
static void collect_candidates(const char* name);
static void find_heap_regions(void);
static uint64_t resolve_class(const char* name);
static uint64_t resolve_class_api(const char* name);
static int  g_batch_state = 0;   /* 0 unknown, 1 batch works, -1 unsupported */

/* Eight scanners saturate every core the title runs on and the game visibly
 * hitches; four is still fast enough for a full sweep and leaves headroom. */
#define SCAN_THREADS 4

static uint64_t g_name_lo = 0x13800000000ULL, g_name_hi = 0x13A00000000ULL;
static uint64_t g_inst_lo = 0x13A00000000ULL, g_inst_hi = 0x13E00000000ULL;

static uint64_t g_module = 0;
static uint64_t g_cam_obj = 0;
static uint64_t g_k_cam = 0;
static uint64_t g_k_char = 0;
static float    g_fov = 60.0f;
static DWORD    g_last_scan = 0;
static uint64_t g_anchor = 0;       /* first character of the last scan    */
static char     g_status[256] = "idle";

typedef struct { float p[4], q[4], s[4]; } MESH_TRS;
typedef struct {
    uint64_t obj, tr, skel;
    unsigned gen;
    RR_V3 hidden_pos;
    RR_V3 hidden_anchor;
    int hidden_in_object;
    uint64_t mesh_h;
    int      mesh_idx[5];
    uint64_t mesh_native[5];
    int      mesh_root;             /* mesh_idx[0] is the movement root, not the head */
    int      mesh_first, mesh_span;
    int      cham_idx[RR_CHAM_BONES];
    uint64_t cham_native[RR_CHAM_BONES];
    int      cham_ready;
    DWORD    mesh_view_at;          /* last fully validated view mapping */
    int32_t  id;
    int      armor, crouch, local;
    int      inveh, vehicle_id;
    float    dist;
    char     role[20];
    char     clan_tag[64];
    char     weapon[20];
    char     veh[20];
    char     name[RR_NAME_MAX];
} ENT_CACHE;
static void cache_cham_rig(ENT_CACHE* e, const ENT_CACHE* previous);

/* Per-character pose cache and its last use time. */
typedef struct {
    uint64_t obj;
    DWORD    t;
    int      ok;
    uint64_t cham_root;
    DWORD    cham_time;
    RR_V3    cham_origin;
    float    cham_skin[RR_CHAM_BONES][12];
    uint64_t mesh_h, mesh_head;
    DWORD    mesh_time;
    int      mesh_valid, mesh_cham_valid, mesh_cham_ready;
    int      mesh_nbones, mesh_full_rig, mesh_group[4];
    float    diag_quat[2][3][4];
    int      diag_quat_count;
    float    diag_rotation_history[3][8][4];
    int      diag_rotation_count;
    float    cham_yaw;
    unsigned cham_hold_since;
    int      cham_hold_pending;
    RR_V3    mesh_origin;
    float    mesh_bone[RR_MAX_BONES][3];
    RR_V3    mesh_feet, mesh_top;
    RR_POSE_GATE arm_gate[2];
    RR_POSE_GATE torso_gate;
    RR_POSE_GATE head_gate;
    MESH_TRS torso_trs[4];
    MESH_TRS head_trs[RR_RIG_L_ARM_0_JNT-RR_RIG_M_HEAD_1_JNT];
    MESH_TRS arm_trs[RR_RIG_R_ARMTWIST_JNT-RR_RIG_L_ARM_0_JNT+1];
} MOTION;
#define MOTION_SLOTS 96
static MOTION g_motion[MOTION_SLOTS];

static MOTION* motion_slot(uint64_t obj) {
    int free_i = -1;
    for (int i = 0; i < MOTION_SLOTS; i++) {
        if (g_motion[i].ok && g_motion[i].obj == obj) return &g_motion[i];
        if (!g_motion[i].ok && free_i < 0) free_i = i;
    }
    if (free_i < 0) {
        free_i = 0;
        for (int i = 1; i < MOTION_SLOTS; i++)
            if (g_motion[i].t < g_motion[free_i].t) free_i = i;
    }
    memset(&g_motion[free_i], 0, sizeof(MOTION));
    g_motion[free_i].obj = obj;
    return &g_motion[free_i];
}

static ENT_CACHE g_cache[RR_MAX_PLAYERS];   /* published snapshot            */
static ENT_CACHE g_stage[RR_MAX_PLAYERS];   /* scan target                   */
#define RR_MAX_VEHICLES 128
#define RR_VEHICLE_POSE_DEPTH 8
typedef struct {
    uint64_t obj, klass, tr, marker_transform, marker_native, marker_h;
    uint64_t render_trs[RR_VEHICLE_POSE_DEPTH];
    int render_depth;
    uint64_t render_transform, render_native, render_hierarchy;
    int render_index;
    DWORD render_view_at;
    int32_t id; int kind, occupants, occupants_known, marker_idx;
    char name[RR_NAME_MAX];
    char role[20];
} VEH_CACHE;
static VEH_CACHE g_vehicles[RR_MAX_VEHICLES], g_vehicles_stage[RR_MAX_VEHICLES];
static int g_nvehicles;
static uint64_t g_vehicle_world;
static unsigned g_vehicle_generation;
#define RR_MAX_LOOT 192
typedef struct {
    int32_t id;
    int kind, rarity;
    RR_V3 pos;
    char name[RR_NAME_MAX];
    /* Addresses retained from the validated loot chain for later write
     * requests. Zero means unavailable. Revalidate before each write because
     * an object can move or be recycled after the cache is published.
     */
    uint64_t obj;        /* LootNodeModel / RandomLootObjectModel            */
    uint64_t pos_addr;   /* the Vector3 position inside its description      */
    uint64_t flag_addr;  /* the BoolData payload: taken (kind 1) / empty (2) */
    /* Retain the resolved description objects for diagnostics so a dump uses
     * the same chain that produced the displayed entry.
     */
    uint64_t desc;       /* LocationLootNodeDescription / RandomLootObject.. */
    uint64_t loot;       /* LootNodeLootDescription / LootObjectItemDesc..   */
    uint64_t inv;        /* InventoryItemDescription                         */
} LOOT_CACHE;
static LOOT_CACHE g_loot[2][RR_MAX_LOOT];
static int g_nloot[2];
static uint64_t g_loot_world[2];
static DWORD g_loot_stamp[2];
#define RR_MAX_WORLD_MARKERS 256
static LOOT_CACHE g_world_markers[RR_MAX_WORLD_MARKERS];
static int g_nworld_markers;
static uint64_t g_markers_world;
static DWORD g_markers_stamp;
static void cache_world_markers(void);
static int       g_ncache = 0;
static int       g_stage_n = 0;
static CRITICAL_SECTION g_cache_cs;
static int       g_cs_ready = 0;
static volatile LONG g_worker_started = 0;

typedef RR_HIDE_OBJECT HIDE_OBJECT;
static HIDE_OBJECT g_hide_objects[RR_MAX_HIDE_OBJECTS];
static RR_HIDE_SNAPSHOT g_hide_snapshot;
static int g_nhide_objects;
static uint64_t g_hide_world;
static DWORD g_hide_stamp;
static void cache_hide_objects(void);

/* Status. */

static void set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

void game_status(char* out, size_t n) {
    if (!out || !n) return;
    LONG period = InterlockedCompareExchange(&g_tick_period, 0, 0);
    if (period > 0) snprintf(out, n, "%s tick=%ldms", g_status, period);
    else snprintf(out, n, "%s timing=unavailable", g_status);
}
int  game_instance_mode(void) { return 1; }
uint64_t game_world_addr(void) { return g_world; }
int  game_prim_offset(void) { return 0; }
int  game_dict_offset(void) { return 0x10; }
uint64_t game_lens_debug(void) { return g_aim_updater; }

/* Runtime file. */

static void runtime_save(void) {
    /* Do not replace a usable runtime cache with a failed discovery result;
     * otherwise the next launch can inherit invalid anchors and narrowed ranges.
     */
    FILE* f;
    if (!PL(g_world) || !PL(g_k_world)) return;

    f = fopen(RUNTIME_FILE, "w");
    if (!f) return;
    fprintf(f, "# written by RobRunESP; delete to force a rescan\n");
    fprintf(f, "class_camera=0x%llX\n", (unsigned long long)g_k_cam);
    fprintf(f, "class_char=0x%llX\n", (unsigned long long)g_k_char);
    fprintf(f, "class_world=0x%llX\n", (unsigned long long)g_k_world);
    fprintf(f, "pid=%u\n", (unsigned)g_rr_pid);
    fprintf(f, "world=0x%llX\n", (unsigned long long)g_world);
    fprintf(f, "camera=0x%llX\n", (unsigned long long)g_cam_obj);
    fprintf(f, "fov_x100=%d\n", (int)(g_fov * 100.0f));
    fprintf(f, "scan_name_lo=0x%llX\n", (unsigned long long)g_name_lo);
    fprintf(f, "scan_name_hi=0x%llX\n", (unsigned long long)g_name_hi);
    fprintf(f, "scan_inst_lo=0x%llX\n", (unsigned long long)g_inst_lo);
    fprintf(f, "scan_inst_hi=0x%llX\n", (unsigned long long)g_inst_hi);
    fclose(f);
}

static void runtime_load(void) {
    FILE* f = fopen(RUNTIME_FILE, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* k = line;
        const char* v = eq + 1;
        if (!strcmp(k, "class_camera")) g_k_cam = strtoull(v, NULL, 0);
        else if (!strcmp(k, "class_char")) g_k_char = strtoull(v, NULL, 0);
        else if (!strcmp(k, "class_world")) g_k_world = strtoull(v, NULL, 0);
        else if (!strcmp(k, "pid")) g_saved_pid = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(k, "world")) g_saved_world = strtoull(v, NULL, 0);
        else if (!strcmp(k, "camera")) g_cam_obj = strtoull(v, NULL, 0);
        else if (!strcmp(k, "fov_x100")) { int _i = atoi(v); g_fov = (float)_i / 100.0f; }
        else if (!strcmp(k, "scan_name_lo")) g_name_lo = strtoull(v, NULL, 0);
        else if (!strcmp(k, "scan_name_hi")) g_name_hi = strtoull(v, NULL, 0);
        else if (!strcmp(k, "scan_inst_lo")) g_inst_lo = strtoull(v, NULL, 0);
        else if (!strcmp(k, "scan_inst_hi")) g_inst_hi = strtoull(v, NULL, 0);
    }
    fclose(f);
    if (g_fov < 20.0f || g_fov > 140.0f) g_fov = 60.0f;

    /* If the cached scan found no world, keep candidate classes but discard
     * the failed address-range narrowing so the next scan can search broadly.
     */
    if (!PL(g_k_world) && !PL(g_saved_world)) {
        g_k_world = 0;
        g_saved_world = 0;
        g_saved_pid = 0;
        g_name_lo = g_name_hi = 0;
        g_inst_lo = g_inst_hi = 0;
    }
}

/* Validation. */

static int class_named(uint64_t k, const char* want) {
    if (!PL(k)) return 0;
    char nm[128];
    if (!u_class_name(k, nm, sizeof(nm))) return 0;
    return !_stricmp(nm, want);
}

static int camera_validate(uint64_t c) {
    if (!PL(c) || !class_named(u_klass(c), "BattleCameraModel")) return 0;
    RR_V3 fwd, pos;
    if (!rr_read(c + RR_OFF_CAM_FWD, &fwd, sizeof(fwd))) return 0;
    if (!rr_read(c + RR_OFF_CAM_POS, &pos, sizeof(pos))) return 0;
    float len = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    if (!(len > 0.9f && len < 1.1f)) return 0;
    if (!isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z)) return 0;
    return 1;
}

/* 1 = plausible, 0 = stale/invalid, -1 = local player unavailable. */
static int camera_near_local(uint64_t c) {
    if (!camera_validate(c)) return 0;
    uint64_t local_tr = 0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    for (int i = 0; i < g_ncache; ++i) {
        if (g_cache[i].local) { local_tr = g_cache[i].tr; break; }
    }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    float lp[3], cp[3];
    if (!PL(local_tr) || !rr_read(local_tr + RR_OFF_CTM_X, lp, sizeof(lp))) return -1;
    if (!rr_read(c + RR_OFF_CAM_POS, cp, sizeof(cp))) return 0;
    float x = cp[0]-lp[0], y = cp[1]-lp[1], z = cp[2]-lp[2];
    return x*x + y*y + z*z < 30.0f*30.0f;
}

/* Class discovery. */

static int ascii_at(uint64_t ptr, char* out, size_t n) {
    out[0] = 0;
    if (!PL(ptr)) return 0;
    static const size_t tries[] = { 96, 64, 32, 16, 8 };
    char tmp[96];
    int ok = 0;
    for (size_t t = 0; t < sizeof(tries) / sizeof(tries[0]); t++)
        if (rr_read(ptr, tmp, tries[t])) { ok = 1; break; }
    if (!ok) return 0;
    size_t i = 0;
    while (i + 1 < sizeof(tmp) && i + 1 < n && tmp[i]) {
        unsigned char c = (unsigned char)tmp[i];
        if (c < 32 || c > 126) return 0;
        out[i++] = tmp[i];
    }
    out[i] = 0;
    return i > 0;
}

/* UnityPlayer's GOM and native component layout are build-specific. Treat the
 * dump as a hint: bound every walk and verify all managed types and links. */
#define RR_UNITY_GOM_RVA 0x2230768u
typedef struct { uint64_t world, battle, camera, battle_model, views; } ENTRY_ROOTS;
static int views_updater_valid(uint64_t updater, uint64_t battle);

static uint64_t find_updater_views(uint64_t updater, uint64_t battle, unsigned depth, unsigned* budget) {
    if (!PL(updater) || depth == 0 || !*budget) return 0;
    --*budget;
    if (views_updater_valid(updater, battle)) return updater;
    const uint64_t klass = rr_u64(updater);
    if (class_named(klass, "BattleUpdater")) {
        const uint64_t views = rr_u64(updater + RR_OFF_BATTLE_UPDATER_VIEWS);
        return views_updater_valid(views, battle) ? views : 0;
    }
    if (class_named(klass, "BattleScreenUpdater"))
        return find_updater_views(rr_u64(updater + RR_OFF_BATTLE_SCREEN_UPDATER), battle, depth - 1, budget);
    unsigned list_offset;
    if (class_named(klass, "BattleScreensUpdater")) list_offset = RR_OFF_BATTLE_SCREENS_LIST;
    else if (class_named(klass, "CompositeUpdater")) list_offset = RR_OFF_COMPOSITE_UPDATERS;
    else return 0;
    uint64_t data = 0;
    const int count = u_list(rr_u64(updater + list_offset), &data);
    for (int i = 0; i < count && *budget && i < 128; ++i) {
        const uint64_t views = find_updater_views(rr_u64(data + (uint64_t)i * 8), battle, depth - 1, budget);
        if (views) return views;
    }
    return 0;
}

/* Where the scene walk stopped on the last pass: 0 never ran, 1 no UnityEntry,
 * 2 Game missing, 3 not a GameModel, 4 not a BattleModel, 5 not a
 * BattleEntitiesModel, 6 not a WorldEntitiesModel, 7 resolved. */
static volatile LONG g_diag_stage;
static volatile LONG g_diag_roster = -1;
static volatile LONG g_diag_hits;
static volatile LONG g_init_tries;
/* Roster channel verdicts: 0 not run, 1 class mismatch, 2 roster field null,
 * 3 not a Dictionary, 4 dictionary empty, 5 dictionary unreadable, 6 entries
 * unreadable, 7 filled. wf=world dictionary, bf=battle dictionary. */
static volatile LONG g_diag_wf;
static volatile LONG g_diag_bf;

/* Last place a worker or the loader entered, so a crash handler can name the
 * step instead of just a faulting address. Values are only meaningful next to
 * the GAME_PHASE_* list in rr_game.h. */
static volatile LONG g_phase;
void game_phase_set(int phase) { InterlockedExchange(&g_phase, phase); }
int game_phase_get(void) { return (int)InterlockedCompareExchange(&g_phase, 0, 0); }

static uint64_t entry_component(uint64_t object) {
    uint64_t array = rr_u64(object + 0x20);
    int count = rr_i32(object + 0x30);
    if (!PL(array) || count < 1 || count > 256) return 0;
    for (int i = 0; i < count; ++i) {
        uint64_t native = rr_u64(array + (uint64_t)i * 0x10 + 8);
        if (!PL(native)) continue;
        uint64_t handle = rr_u64(native + 0x18) & ~7ULL;
        uint64_t managed = PL(handle) ? rr_u64(handle) : 0;
        if (u_is(managed, "UnityEntry")) return managed;
        managed = rr_u64(native + 0x28);
        if (u_is(managed, "UnityEntry")) return managed;
    }
    return 0;
}

/* Walk the GOM for the UnityEntry component and its Game object. The cursor
 * model hangs off Game and exists in a lobby, where the battle validation in
 * entry_roots() fails, so the menu must be able to reach it on its own. */
static int unity_entry_game(uint64_t* entry_out, uint64_t* game_out) {
    uint64_t module = 0; uint32_t size = 0;
    if (entry_out) *entry_out = 0;
    if (game_out) *game_out = 0;
    if (!rr_module("UnityPlayer.dll", &module, &size) ||
        size < RR_UNITY_GOM_RVA + 8) return 0;
    uint64_t gom = rr_u64(module + RR_UNITY_GOM_RVA);
    if (!PL(gom)) return 0;
    const unsigned lists[] = { 0x18, 0x28, 0x08 };
    for (unsigned l = 0; l < sizeof(lists)/sizeof(lists[0]); ++l) {
        uint64_t node = rr_u64(gom + lists[l]);
        uint64_t end = rr_u64(gom + lists[l] - 8);
        for (int n = 0; PL(node) && node != end && n < 4096; ++n) {
            uint64_t object = rr_u64(node + 0x10);
            char name[64];
            if (PL(object) && ascii_at(rr_u64(object + 0x50), name, sizeof(name)) &&
                !strcmp(name, "UnityEntry")) {
                uint64_t entry = entry_component(object);
                uint64_t game = PL(entry) ? rr_u64(entry + RR_OFF_ENTRY_GAME) : 0;
                /* Several scene objects can carry a UnityEntry; a stale one must
                 * not end the walk before the live Game is reached. */
                if (!u_is(game, "Game")) continue;
                if (entry_out) *entry_out = entry;
                if (game_out) *game_out = game;
                InterlockedExchange(&g_diag_stage, 7);
                return 1;
            }
            uint64_t next = rr_u64(node + 8);
            if (next == node) break;
            node = next;
        }
    }
    return 0;
}

static int entry_roots(ENTRY_ROOTS* roots) {
    memset(roots, 0, sizeof(*roots));
    uint64_t entry = 0, game = 0;
    if (!unity_entry_game(&entry, &game)) return 0;
    InterlockedExchange(&g_diag_stage, 3);
    uint64_t model = rr_u64(game + RR_OFF_GAME_MODEL);
    if (!u_is(model, "GameModel")) model = rr_u64(game + RR_OFF_GAME_MODEL_LEGACY);
    uint64_t battle_model = u_is(model, "GameModel") ? rr_u64(model + RR_OFF_GAME_BATTLE) : 0;
    if (!u_is(battle_model, "BattleModel")) return 0;
    InterlockedExchange(&g_diag_stage, 4);
    uint64_t battle = rr_u64(battle_model + RR_OFF_BATTLE_ENTITIES);
    uint64_t world = u_is(battle, "BattleEntitiesModel") ? rr_u64(battle + RR_OFF_BATTLE_WORLD) : 0;
    if (!u_is(world, "WorldEntitiesModel")) return 0;
    InterlockedExchange(&g_diag_stage, 5);
    roots->world = world;
    roots->battle = battle;
    roots->battle_model = battle_model;
    uint64_t processor = rr_u64(game + RR_OFF_GAME_START_PROCESSOR);
    if (u_is(processor, "StartGameProcessor")) {
        uint64_t manager = rr_u64(processor + RR_OFF_START_SCREEN_MANAGER);
        unsigned budget = 128;
        if (u_is(manager, "ScreenManager"))
            roots->views = find_updater_views(rr_u64(manager + RR_OFF_SCREEN_CURRENT_UPDATER), battle, 8, &budget);
    }
    uint64_t camera = rr_u64(battle_model + RR_OFF_BATTLE_CAMERA);
    if (camera_validate(camera)) roots->camera = camera;
    return 1;
}

int game_entry_probe(uint64_t* world, uint64_t* battle, uint64_t* camera) {
    ENTRY_ROOTS roots;
    int ok = entry_roots(&roots);
    if (world) *world = roots.world;
    if (battle) *battle = roots.battle;
    if (camera) *camera = roots.camera;
    return ok;
}

/* ---- cursor lock ---------------------------------------------------------
 * Game -> BuiltInModel -> CursorLockModel, driven through the game's own
 * Add/Remove methods so its input path owns the OS cursor state (mouse-look
 * stops, cursor reappears, clicks land on the menu). RVAs are from the
 * 26615022 dump; both methods are plain instance calls (rcx=this, rdx=key). */
#define RR_OFF_GAME_BUILTIN      0xC8u    /* Game.Game.BuiltInModel            */
#define RR_OFF_BUILTIN_CURSOR    0x18u    /* BuiltInModel.CursorLockModel      */
#define RR_RVA_CURSOR_ADD        0xF08F80u/* CursorLockModel.Add(key, mode)    */
#define RR_RVA_CURSOR_REMOVE     0xF08FF0u/* CursorLockModel.Remove(key)       */
#define RR_CURSOR_KEY_SIDE_MENU  7        /* CursorLockKey.SideMenu            */
#define RR_CURSOR_MODE_NONE      0        /* CursorLockMode.None               */

typedef void (*CURSOR_ADD_FN)(uint64_t self, uint64_t key, uint64_t mode);
typedef void (*CURSOR_REMOVE_FN)(uint64_t self, uint64_t key);

static volatile LONG g_cursor_locked;

static uint64_t cursor_lock_model(void) {
    uint64_t entry = 0, game = 0;
    if (!unity_entry_game(&entry, &game)) return 0;
    uint64_t builtin = rr_u64(game + RR_OFF_GAME_BUILTIN);
    uint64_t model = PL(builtin) ? rr_u64(builtin + RR_OFF_BUILTIN_CURSOR) : 0;
    return u_is(model, "CursorLockModel") ? model : 0;
}

/* Add/Remove allocate or grow the game's SortedList, which is only safe from
 * a thread the GC knows about; the render thread is not one. */
static void cursor_attach_thread(HMODULE game_module) {
    typedef void* (*DOMAIN_GET_FN)(void);
    typedef void* (*ATTACH_FN)(void*);
    DOMAIN_GET_FN domain_get = (DOMAIN_GET_FN)(void*)GetProcAddress(game_module, "il2cpp_domain_get");
    ATTACH_FN attach = (ATTACH_FN)(void*)GetProcAddress(game_module, "il2cpp_thread_attach");
    if (!domain_get || !attach) return;
    void* domain = domain_get();
    if (domain) attach(domain);
}

int game_cursor_menu(int open) {
    HMODULE game_module = GetModuleHandleA("GameAssembly.dll");
    if (!game_module) return 0;
    /* Never owned the lock: report "no game lock" so the caller falls back to
     * the Win32 clip path instead of assuming the game restored the cursor. */
    if (!open && !InterlockedCompareExchange(&g_cursor_locked, 0, 0)) return 0;
    uint64_t model = cursor_lock_model();
    if (!model) return 0;
    cursor_attach_thread(game_module);
    const uintptr_t base = (uintptr_t)game_module;
    int ok = 0;
    /* A duplicate Add raises a managed exception; the SEH frame turns that
     * into a failed call instead of unwinding into the render thread. */
    __try {
        if (open) {
            ((CURSOR_ADD_FN)(base + RR_RVA_CURSOR_ADD))(model, RR_CURSOR_KEY_SIDE_MENU, RR_CURSOR_MODE_NONE);
            InterlockedExchange(&g_cursor_locked, 1);
        } else {
            ((CURSOR_REMOVE_FN)(base + RR_RVA_CURSOR_REMOVE))(model, RR_CURSOR_KEY_SIDE_MENU);
            InterlockedExchange(&g_cursor_locked, 0);
        }
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    return ok;
}

static int class_plausible(uint64_t k, const char* want) {
    /* Do not require an image or namespace string: valid global-namespace
     * classes may lack them. Check the class name here and validate discovered
     * instances separately.
     */
    if (!PL(k)) return 0;
    return class_named(k, want);
}

/* Character scan. */

/* Resolve only a live, explicitly referenced hiding object in this world.
 * Managed field offsets are generated from the supplied CppSDK; replication of this
 * state for other players still needs live verification. */
static int hidden_object_find(uint64_t character, HIDE_OBJECT* out) {
    uint64_t world = g_world;
    if (!PL(world) || rr_u64(world) != g_k_world) return 0;
    uint64_t state = rr_u64(character + RR_OFF_CHAR_STATE);
    if (!u_is(state, "BattleCharacterStateModel")) return 0;
    uint64_t hidden = rr_u64(state + RR_OFF_STATE_HIDDEN);
    if (!u_is(hidden, "CharacterHideObjectStateModel")) return 0;
    uint64_t data = rr_u64(hidden + RR_OFF_HIDDEN_ID_DATA);
    if (!u_is(data, "BattleEntityIdData") && !u_is(data, "IntData")) return 0;
    int32_t id = -1;
    if (!rr_read(data + RR_OFF_PRIM_VALUE, &id, sizeof(id)) || id < 0) return 0;
    uint64_t dict = rr_u64(world + RR_OFF_WEM_HIDE_OBJECTS), entries = 0;
    int n = 0, stride = 0;
    if (!u_dict(dict, &n, &entries, &stride) || n > 4096 || stride != 0x18) return 0;
    for (int i = 0; i < n; ++i) {
        uint8_t entry[0x18]; int32_t key,hash; uint64_t obj;
        if (!rr_read(entries + (uint64_t)i * 0x18, entry, sizeof(entry))) return 0;
        memcpy(&key, entry + 8, sizeof(key));
        memcpy(&hash,entry,4);if(hash<0)continue;
        memcpy(&obj, entry + 0x10, sizeof(obj));
        if (key != id || !u_is(obj, "HideObjectModel")) continue;
        return rr_hide_object_read(obj,id,out)&&world==g_world&&rr_u64(world)==g_k_world;
    }
    return 0;
}
static int hidden_object_position(uint64_t character,RR_V3* out) {
    HIDE_OBJECT object;if(!hidden_object_find(character,&object))return 0;
    *out=object.pos;return 1;
}

static volatile LONG g_world_gen;

static unsigned world_gen(void) {
    return (unsigned)InterlockedCompareExchange(&g_world_gen, 0, 0);
}

static void fill_entry(ENT_CACHE* c, uint64_t p, unsigned gen) {
    memset(c, 0, sizeof(*c));
    c->gen = gen;
    c->obj = p;
    c->armor = -1;
    c->crouch = -1;
    c->vehicle_id = -1;
    c->local = PL(rr_u64(p + RR_OFF_CHAR_PBM));

    uint8_t head[0x60];
    if (!rr_read(p, head, sizeof(head))) { c->obj = 0; return; }
    /* Recheck the class pointer before using a cached object. Freed objects
     * can retain readable bytes until the allocator reuses their storage.
     */
    {
        uint64_t klass = 0;
        memcpy(&klass, head, 8);
        if (klass != g_k_char) { c->obj = 0; return; }
    }
    memcpy(&c->id, head + RR_OFF_CHAR_ID, 4);
    memcpy(&c->tr, head + RR_OFF_CHAR_TRANSFORM, 8);
    /* SkeletonModel is at +0x138, outside the 0x60-byte header read. */
    c->skel = rr_u64(p + RR_OFF_CHAR_SKELETON);
    if (!c->local || br_get()->show_self || br_local_esp_enabled(br_get())) {
        HIDE_OBJECT hidden;
        if(hidden_object_find(p,&hidden)) {
            c->hidden_in_object=1;c->hidden_pos=hidden.pos;c->hidden_anchor=hidden.anchor;
        }
    }
    if (!PL(c->tr) && !c->hidden_in_object) { c->obj = 0; return; }

    uint64_t data = rr_u64(p + RR_OFF_CHAR_DATA);
    if (PL(data)) {
        uint64_t common = rr_u64(data + RR_OFF_DATA_COMMON);
        uint64_t pub = rr_u64(data + RR_OFF_DATA_PUBLIC);
        if (PL(common)) {
            uint64_t ref = rr_u64(common + RR_OFF_COMMON_ROLE);
            uint64_t desc = PL(ref) ? rr_u64(ref + RR_OFF_REF_VALUE) : 0;
            uint64_t str = PL(desc) ? rr_u64(desc + RR_OFF_DESC_NAME) : 0;
            if (PL(str)) u_string(str, c->role, sizeof(c->role));
            c->role[sizeof(c->role) - 1] = 0;
            /* Require a replicated role to reject templates and unspawned objects
             * that would otherwise appear as stationary markers. The local
             * player's own role is not replicated until the server assigns one,
             * so keeping that rule for it hid the self marker on civilians.
             */
            if (!c->role[0] && !c->local) { c->obj = 0; return; }
            /* The item in hands is replicated for remote characters. */
            ref = rr_u64(common + RR_OFF_COMMON_ITEM_IN_HANDS);
            desc = PL(ref) ? rr_u64(ref + RR_OFF_REF_VALUE) : 0;
            str = PL(desc) ? rr_u64(desc + RR_OFF_DESC_NAME) : 0;
            if (PL(str)) u_string(str, c->weapon, sizeof(c->weapon));
        }
        if (PL(pub)) {
            uint64_t bd = rr_u64(pub + RR_OFF_PUBLIC_ARMOR);
            if (PL(bd)) { uint8_t b = 0; if (rr_read(bd + RR_OFF_PRIM_VALUE, &b, 1)) c->armor = b; }
        }
    }
    uint64_t wm = rr_u64(p + RR_OFF_CHAR_WEAPON);
    if (!c->weapon[0] && PL(wm)) {
        uint64_t eq = rr_u64(wm + RR_OFF_WEAPON_ITEM);
        uint64_t ref = PL(eq) ? rr_u64(eq + RR_OFF_WEAPON_ITEM) : 0;
        uint64_t desc = PL(ref) ? rr_u64(ref + RR_OFF_REF_VALUE) : 0;
        uint64_t str = PL(desc) ? rr_u64(desc + RR_OFF_DESC_NAME) : 0;
        if (PL(str)) u_string(str, c->weapon, sizeof(c->weapon));
        c->weapon[sizeof(c->weapon) - 1] = 0;
    }
    /* seated in a vehicle: InVehicleModel._entityId is -1 when on foot */
    uint64_t iv = rr_u64(p + RR_OFF_CHAR_INVEH);
    if (PL(iv)) {
        uint64_t eid = rr_u64(iv + RR_OFF_INVEH_ID);
        int v = -1;
        if (PL(eid) && !rr_read(eid + 0x18, &v, sizeof(v))) v = -1;
        c->inveh = (v >= 0) ? 1 : 0;
        c->vehicle_id = v;
        if (c->inveh) {
            uint64_t vt = rr_u64(iv + RR_OFF_INVEH_TYPE);
            uint64_t d = PL(vt) ? rr_u64(vt + 0x18) : 0;
            uint64_t s = PL(d) ? rr_u64(d + 0x10) : 0;
            if (PL(s)) u_string(s, c->veh, sizeof(c->veh));
        }
    }
    uint64_t cr = rr_u64(p + RR_OFF_CHAR_CROUCH);
    if (PL(cr)) {
        uint64_t bd = rr_u64(cr + RR_OFF_CROUCH_BOOL);
        if (PL(bd)) { uint8_t b = 0; if (rr_read(bd + RR_OFF_PRIM_VALUE, &b, 1)) c->crouch = b; }
    }
}

typedef struct { uint64_t lo, hi; unsigned gen; } SCAN_CTX;
static volatile LONG g_scan_idx;   /* shared across regions in one pass */

#define RR_MAX_REGIONS 32768
#define RR_MAX_HEAP 512
typedef struct { uint64_t base, size; } RR_HEAP;
typedef struct { uint64_t world, battle; int nregions; RR_HEAP regions[RR_MAX_REGIONS]; } BATTLE_SCAN;
static volatile LONG g_battle_scan_busy = 0;
static HANDLE g_views_ready_event;
void game_scene_debug(uint64_t* battle, uint64_t* updater, int* scanning) {
    if (battle) *battle=g_battle;
    if (updater) *updater=g_views_updater;
    if (scanning) *scanning=(int)InterlockedCompareExchange(&g_battle_scan_busy,0,0);
}
static char g_dbg[160] = "?";
static RR_HEAP g_regions[RR_MAX_REGIONS];
static int     g_nregions;
static RR_HEAP g_heap[RR_MAX_HEAP];
static int     g_nheap;
/* Publish a new live world and drop the published caches that were built from
 * the old one. A death or a map change frees the scene's native transform
 * arrays, material instances and view objects; the addresses stay mapped and
 * readable, so a stale row validates clean and then feeds freed memory to the
 * renderer. The roster is what has to go - the handle caches below it
 * re-validate themselves on every use, so zeroing them only costs recovery
 * time and leaves the rig pass without a views dictionary after every switch. */
static void world_switch(uint64_t next) {
    const uint64_t was = g_world;
    /* Same identity: nothing resolved against it went stale, and clearing here
     * would blank the roster plus the views dictionary the rig pass needs on
     * every pass that merely finds the world unreadable for a moment. */
    if (next == was) return;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    InterlockedIncrement(&g_world_gen);
    g_ncache = 0;
    g_stage_n = 0;
    g_nvehicles = 0;
    g_nloot[0] = 0;
    g_nloot[1] = 0;
    g_nworld_markers = 0;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    /* These three belong to the scene that just went away; the views updater
     * and aim updater point into its object graph, and the camera handle is an
     * object that the unload destroys. */
    InterlockedExchange64((volatile LONG64*)&g_views_updater, 0);
    InterlockedExchange64((volatile LONG64*)&g_aim_updater, 0);
    g_cam_obj = 0;
    g_world = next;
    if (next) g_k_world = u_klass(next);
    if (PL(was)) g_saved_world = was;
    g_hide_world = 0;
    g_loot_world[0] = g_loot_world[1] = 0;
    g_markers_world = 0;
    g_vehicle_world = 0;
    ++g_vehicle_generation;
}

static DWORD WINAPI char_scan_thread(LPVOID param) {
    SCAN_CTX* s = (SCAN_CTX*)param;
    uint8_t buf[0x10000];
    unsigned chunks = 0;
    for (uint64_t a = s->lo; a < s->hi; a += sizeof(buf)) {
        size_t n = (size_t)((s->hi - a) < sizeof(buf) ? (s->hi - a) : sizeof(buf));
        /* Eight sweep threads on the shared default pool can starve the title
         * for seconds per pass; a one-tick yield every 4 MB hands the cores
         * back without changing how much ground is covered. */
        if ((++chunks & 63) == 0) Sleep(1);
        if (!rr_read(a, buf, n)) continue;
        for (size_t i = 0; i + 8 <= n; i += 8) {
            uint64_t v;
            memcpy(&v, buf + i, 8);
            if (v != g_k_char) continue;
            uint64_t p = a + i;
            if (!PL(rr_u64(p))) continue;
            LONG idx = InterlockedIncrement(&g_scan_idx) - 1;   /* shared across regions */
            if (idx < RR_MAX_PLAYERS) {
                fill_entry(&g_stage[idx], p, s->gen);
            }
        }
    }
    return 0;
}

static int scan_window(uint64_t lo, uint64_t hi) {
    if (lo >= hi) return 0;
    LONG before = g_scan_idx;          /* the shared counter is the tally */
    SCAN_CTX s[SCAN_THREADS];
    HANDLE th[SCAN_THREADS];
    const unsigned gen = world_gen();
    const uint64_t words = (hi - lo) / 8;
    for (int i = 0; i < SCAN_THREADS; i++) {
        s[i].lo = lo + 8 * (words * (uint64_t)i / SCAN_THREADS);
        s[i].hi = lo + 8 * (words * (uint64_t)(i + 1) / SCAN_THREADS);
        s[i].gen = gen;
        th[i] = rr_thread_start(char_scan_thread, &s[i]);
    }
    /* A job that never started has to run here: the ranges above live on this
     * stack frame and are gone the moment the function returns. Waiting on a
     * NULL handle would fail instantly and leave a pool worker walking freed
     * stack memory. */
    for (int i = 0; i < SCAN_THREADS; i++)
        if (!th[i]) char_scan_thread(&s[i]);
    for (int i = 0; i < SCAN_THREADS; i++)
        if (th[i]) WaitForSingleObject(th[i], INFINITE);
    for (int i = 0; i < SCAN_THREADS; i++)
        if (th[i]) CloseHandle(th[i]);
    return (int)(g_scan_idx - before);
}

static int cache_from_world(void);
static int cache_from_battle(void);
static uint64_t find_world_instance(void);
static DWORD WINAPI battle_scan_worker(LPVOID p);
static void cache_mesh_views(ENT_CACHE* stage, int count);
static int start_battle_scan(void) {
    if (InterlockedCompareExchange(&g_battle_scan_busy, 1, 0) != 0) return 0;
    BATTLE_SCAN* scan = (BATTLE_SCAN*)malloc(sizeof(*scan));
    if (scan) {
        scan->world = g_world; scan->battle = g_battle; scan->nregions = g_nregions;
        memcpy(scan->regions, g_regions, sizeof(RR_HEAP) * (size_t)g_nregions);
        HANDLE thread = rr_thread_start(battle_scan_worker, scan);
        if (thread) { CloseHandle(thread); return 1; }
        free(scan);
    }
    InterlockedExchange(&g_battle_scan_busy, 0);
    return 0;
}
static void cache_vehicles(void);
static void cache_loot(int kind);

/* The heap sweep walks every readable region; running it every retry stalled
 * the title. Two channels of live dictionaries get tried first, so the sweep
 * only runs when both of them are empty or wrong. */
static DWORD g_heap_scan_at;

static int instance_scan(uint64_t lo, uint64_t hi) {
    if (!g_k_char) return 0;
    InterlockedExchange(&g_diag_roster, -1);
    InterlockedExchange(&g_diag_hits, 0);
    InterlockedExchange(&g_diag_wf, 0);
    InterlockedExchange(&g_diag_bf, 0);

    /* Channel 1: WorldEntitiesModel._battleCharacters. */
    if (PL(g_world) && g_k_world) {
        game_phase_set(GAME_PHASE_ROSTER_WORLD);
        int wc = cache_from_world();
        if (wc > 0) { InterlockedExchange(&g_diag_roster, wc); return wc; }
        if (wc == -2) {
            /* An incomplete read is not proof that the world was destroyed;
             * the previous roster stays published until the next pass. */
            if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
            int count = g_ncache;
            if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
            if (count > 0) { InterlockedExchange(&g_diag_roster, count); return count; }
        } else if (wc == -1) {
            /* A recycled world pointer must never turn into a heap roster,
             * but the battle dictionary below may still be live. */
            world_switch(0);
        }
    }

    /* Channel 2: BattleEntitiesModel._models. Independent of the world's
     * roster field, which this build reports empty in some match states. */
    game_phase_set(GAME_PHASE_ROSTER_BATTLE);
    int bc = cache_from_battle();
    if (bc > 0) { InterlockedExchange(&g_diag_roster, bc); return bc; }

    /* Channel 3: object scan. It also finds pre-allocated, not-spawned
     * characters, so it is the last resort rather than the first read. */
    const DWORD now = GetTickCount();
    if (g_heap_scan_at && now - g_heap_scan_at < 15000) return 0;
    g_heap_scan_at = now ? now : 1;
    game_phase_set(GAME_PHASE_HEAP_SCAN);
    /* The tally is shared across region windows; without this reset a long
     * session leaves it past RR_MAX_PLAYERS, so every window reports hits it
     * no longer writes and the compaction below republishes whatever stale
     * rows happen to still sit in g_stage. */
    g_scan_idx = 0;
    const unsigned gen = world_gen();
    int total = 0;
    if (g_nregions > 0) {
        for (int i = 0; i < g_nregions; i++)
            total += scan_window(g_regions[i].base, g_regions[i].base + g_regions[i].size);
    } else {
        total = scan_window(lo, hi);
    }

    int limit = (total < RR_MAX_PLAYERS) ? total : RR_MAX_PLAYERS;
    InterlockedExchange(&g_diag_hits, total);
    int n = 0;
    for (int i = 0; i < limit; i++) {
        if (!g_stage[i].obj || g_stage[i].gen != gen) continue;
        if (n != i) g_stage[n] = g_stage[i];
        n++;
    }
    g_stage_n = n;

    cache_mesh_views(g_stage, n);
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_cache, g_stage, sizeof(ENT_CACHE) * (size_t)n);
    g_ncache = n;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);

    if (n) g_anchor = g_cache[0].obj;
    return n;
}

static uint64_t find_camera_scan(void);

/* Refresh the roster on a worker thread so long scans do not block
 * position sampling.
 */
static DWORD WINAPI scan_worker(LPVOID p) {
    (void)p;
    DWORD last_full = GetTickCount();
    DWORD last_camera_scan = GetTickCount() - 5000;
    DWORD last_battle_scan = GetTickCount() - 30000;
    if (PL(g_world) && start_battle_scan()) last_battle_scan = GetTickCount();
    DWORD last_world_scan = GetTickCount() - 8000;
    DWORD last_loot_scan = GetTickCount() - 2000;
    DWORD last_markers_scan = GetTickCount() - 2000;
    DWORD last_hide_scan = GetTickCount() - 10000;
    DWORD last_entry_scan = GetTickCount() - 8000;
    DWORD last_entry_success = 0;
    int entry_seen = 0;
    DWORD roster_empty_since = 0;
    DWORD rig_stall_since = 0;
    DWORD last_forced_walk = 0;
    for (;;) {
        /* The live world dictionary is cheap to read. Refresh it promptly so
         * characters entering replication range don't wait four seconds to
         * appear; keep the slower interval for the heap-scan fallback. */
        const DWORD refresh_ms = 500;
        if (g_views_ready_event) WaitForSingleObject(g_views_ready_event, refresh_ms);
        else Sleep(refresh_ms);
        game_phase_set(GAME_PHASE_WORKER_IDLE);
        DWORD now = GetTickCount();
        /* An empty roster no longer forces a full region walk every pass: in a
         * lobby the sweep can never succeed, and running it every four seconds
         * starved the game's own threads. The dictionary channels still run
         * each pass, and instance_scan's 15 s gate covers the sweep. */
        int full = (now - last_full >= 40000);
        /* A level change rebuilds every character object while the world
         * instance itself often survives, so the region hints collected on the
         * previous map point at memory that now holds nothing. The world-find
         * branch further down cannot cover that either: entry_roots keeps
         * succeeding, which keeps it disabled. Watch the published roster
         * instead. Empty for 2.5 s, or present with no rig resolved for 4 s,
         * means the hints are stale and every readable range has to be walked
         * again. */
        {
            int roster = 0, rigs = 0;
            if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
            roster = g_ncache;
            if (roster > RR_MAX_PLAYERS) roster = RR_MAX_PLAYERS;
            for (int i = 0; i < roster; ++i)
                if (g_cache[i].mesh_h) { rigs = 1; break; }
            if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
            if (roster == 0) {
                if (!roster_empty_since) roster_empty_since = now ? now : 1;
            } else {
                roster_empty_since = 0;
            }
            if (roster > 0 && !rigs) {
                if (!rig_stall_since) rig_stall_since = now ? now : 1;
            } else {
                rig_stall_since = 0;
            }
            const int stalled = (roster_empty_since && now - roster_empty_since >= 2500) ||
                                (rig_stall_since && now - rig_stall_since >= 4000);
            if (stalled && (!last_forced_walk || now - last_forced_walk >= 15000)) {
                last_forced_walk = now;
                full = 1;
                /* The point of this pass is the sweep: bypass instance_scan's
                 * 15 s heap gate so the new map is scanned now. */
                g_heap_scan_at = 0;
                roster_empty_since = 0;
                rig_stall_since = 0;
            }
        }

        if (now - last_entry_scan >= 1000) {
            ENTRY_ROOTS roots;
            last_entry_scan = now;
            game_phase_set(GAME_PHASE_ENTRY_WALK);
            if (entry_roots(&roots)) {
                last_entry_success = now;
                entry_seen = 1;
                if (roots.world != g_world) {
                    world_switch(roots.world);
                    last_battle_scan = now - 30000;
                    last_camera_scan = now - 5000;
                    g_heap_scan_at = 0;
                }
                InterlockedExchange64((volatile LONG64*)&g_battle, (LONG64)roots.battle);
                InterlockedExchange64((volatile LONG64*)&g_battle_model,(LONG64)roots.battle_model);
                if (roots.views) InterlockedExchange64((volatile LONG64*)&g_views_updater, (LONG64)roots.views);
                if (roots.camera) g_cam_obj = roots.camera;
            }
        }

        /* Re-validate the class after a scene change; cheap when it holds. */
        if (!g_k_char || !class_named(g_k_char, "BattleCharacterModel")) {
            g_k_char = resolve_class("BattleCharacterModel");
            full = 1;
        }

        /* World objects are recycled during scene changes. Stop publishing the
         * old roster and locate the new one before reading characters again. */
        uint64_t world_class = 0;
        int world_invalid = !PL(g_world);
        if (!world_invalid)
            world_invalid = !rr_read(g_world, &world_class, sizeof(world_class)) ||
                            world_class != g_k_world;
        if (PL(g_world) && world_invalid) {
            world_switch(0);
            g_heap_scan_at = 0;
            last_world_scan = now - 2000;
        }

        if (full) {
            last_full = now;
            game_phase_set(GAME_PHASE_REGION_WALK);
            find_heap_regions();                 /* VAD walk + all regions */
        }
        instance_scan(0, 0);
        now = GetTickCount();
        if (PL(g_world) && now - last_hide_scan >= 500) {
            last_hide_scan = now;
            game_phase_set(GAME_PHASE_HIDE_CACHE);
            cache_hide_objects();
        }
        if (!g_ncache && g_k_world && now - last_world_scan >= 2000 &&
            (!entry_seen || now - last_entry_success >= 12000)) {
            last_world_scan = now;
            game_phase_set(GAME_PHASE_WORLD_FIND);
            find_heap_regions();
            uint64_t current = find_world_instance();
            if (PL(current) && current != g_world) {
                world_switch(current);
                instance_scan(0, 0);
                last_battle_scan = GetTickCount() - 30000;
                last_camera_scan = GetTickCount() - 5000;
            }
        }
        int scan_vehicles = br_get()->show_vehicle_markers;
#ifndef RR_INTERNAL
        scan_vehicles |= br_get()->vehicle_chams;
#endif
#ifdef RR_PORT_DIAGNOSTICS
        scan_vehicles |= br_get()->show_vehicles;
#endif
        if (scan_vehicles) {
            game_phase_set(GAME_PHASE_VEHICLES);
            cache_vehicles();
        }
        if ((br_get()->show_loot || br_get()->show_loot_nodes) && now - last_loot_scan >= 2000) {
            last_loot_scan = now;
            game_phase_set(GAME_PHASE_LOOT);
            if (br_get()->show_loot) cache_loot(1);
            if (br_get()->show_loot_nodes) cache_loot(2);
        }

        if ((br_get()->show_containers || br_get()->show_doors || br_get()->show_dropped_loot || br_get()->show_vehicle_loot) && now - last_markers_scan >= 2000) {
            last_markers_scan = now;
            game_phase_set(GAME_PHASE_MARKERS);
            cache_world_markers();
        }

        /* Resolve the scene's slot table on the background worker, once per
         * world. Validate the world link whenever the cached object is used. */
        now = GetTickCount();
        if (PL(g_views_updater) && PL(g_battle) && !views_updater_valid(g_views_updater, g_battle)) {
            InterlockedExchange64((volatile LONG64*)&g_views_updater, 0);
            last_battle_scan = now - 30000;
        }
        if (PL(g_aim_updater) && (rr_u64(g_aim_updater + RR_OFF_AIM_BATTLE) != g_battle ||
            !class_named(rr_u64(g_aim_updater), "BattleAimTargetUpdater")))
            InterlockedExchange64((volatile LONG64*)&g_aim_updater, 0);
        const DWORD battle_scan_interval = PL(g_views_updater) ? 30000 : 2000;
        if (PL(g_world) && (!PL(g_battle) || rr_u64(g_battle + RR_OFF_BATTLE_WORLD) != g_world ||
            !PL(g_views_updater) || !PL(g_aim_updater)) &&
            now - last_battle_scan >= battle_scan_interval && start_battle_scan()) {
            last_battle_scan = now;
            game_phase_set(GAME_PHASE_BATTLE_SCAN);
        }

        /* The active battle model owns the actual camera; a proximity scan can
         * otherwise select a dormant camera that happens to be nearby. */
        if (PL(g_views_updater) && PL(g_battle) &&
            views_updater_valid(g_views_updater, g_battle) && rr_u64(g_battle + RR_OFF_BATTLE_WORLD) == g_world) {
            uint64_t model = rr_u64(g_views_updater + RR_OFF_VIEWS_MODEL);
            uint64_t timing = rr_u64(model + 0x38);
            if (u_is(timing, "TimeModel")) {
                InterlockedExchange64(&g_time_model, (LONG64)timing);
                InterlockedExchange64(&g_time_world, (LONG64)g_world);
            }
            uint64_t scene_camera = rr_u64(model + RR_OFF_BATTLE_CAMERA);
            if (camera_validate(scene_camera)) g_cam_obj = scene_camera;
        }
        int camera_state = camera_near_local(g_cam_obj);
        now = GetTickCount();
        if (g_ncache && camera_state == 0 && now - last_camera_scan >= 5000) {
            last_camera_scan = now;
            game_phase_set(GAME_PHASE_CAMERA_SCAN);
            find_heap_regions(); /* scene changes can allocate new regions */
            g_cam_obj = find_camera_scan();
            camera_state = camera_near_local(g_cam_obj);
            if (camera_state == 1) runtime_save();
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "characters=%d camera=%s [%s]", g_ncache,
                 camera_state == 1 ? "model" : "local", g_dbg);
        set_status("%s", msg);
    }
    return 0;
}

/* Camera scan. */

typedef struct { uint64_t lo, hi; volatile LONG found; uint64_t addr; } CAM_CTX;

static DWORD WINAPI cam_scan_thread(LPVOID param) {
    CAM_CTX* c = (CAM_CTX*)param;
    uint8_t buf[0x10000];
    unsigned chunks = 0;
    for (uint64_t a = c->lo; a < c->hi; a += sizeof(buf)) {
        if (InterlockedCompareExchange(&c->found, 0, 0)) return 0;
        if ((++chunks & 63) == 0) Sleep(1);
        size_t n = (size_t)((c->hi - a) < sizeof(buf) ? (c->hi - a) : sizeof(buf));
        if (!rr_read(a, buf, n)) continue;
        for (size_t i = 0; i + 8 <= n; i += 8) {
            uint64_t v;
            memcpy(&v, buf + i, 8);
            if (v != g_k_cam) continue;
            uint64_t p = a + i;
            if (camera_validate(p)) { c->addr = p; InterlockedExchange(&c->found, 1); return 0; }
        }
    }
    return 0;
}

static uint64_t find_camera_scan(void) {
    if (!g_k_cam) return 0;

    /* Use distance from the local player as a camera-selection heuristic to
     * reduce matches on dormant preview cameras.
     */
    float lp[3];
    int have_lp = 0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    for (int i = 0; i < g_ncache; i++) {
        if (!g_cache[i].local) continue;
        if (rr_read(g_cache[i].tr + RR_OFF_CTM_X, lp, sizeof(lp))) have_lp = 1;
        break;
    }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    if (!have_lp) return 0;

    uint64_t best = 0;
    float best_d = 1e30f;
    static uint8_t buf[0x10000];
    /* The live camera usually shares the active world's heap neighborhood.
     * Search it first; fall back to all regions when it is elsewhere. */
    for (int pass = 0; pass < 2; ++pass) {
      uint64_t radius = 512ULL << 20;
      uint64_t near_lo = g_world > radius ? g_world - radius : 0;
      uint64_t near_hi = g_world + radius;
      for (int r = 0; r < g_nregions; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        if (pass == 0 && PL(g_world)) {
            if (lo < near_lo) lo = near_lo;
            if (hi > near_hi) hi = near_hi;
            if (lo >= hi) continue;
        } else if (pass == 0) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != g_k_cam) continue;
                uint64_t c = a + i;
                if (!camera_validate(c)) continue;
                float cp[3];
                if (!rr_read(c + RR_OFF_CAM_POS, cp, sizeof(cp))) continue;
                float d;
                if (have_lp) {
                    float dx = cp[0] - lp[0], dy = cp[1] - lp[1], dz = cp[2] - lp[2];
                    d = sqrtf(dx * dx + dy * dy + dz * dz);
                } else {
                    d = 0.0f;              /* no local entity: first valid wins */
                }
                if (d < best_d) { best_d = d; best = c; }
            }
        }
      }
      if (best && best_d < 30.0f) break;
    }
    if (best_d >= 30.0f) best = 0;
    if (best) snprintf(g_dbg, sizeof(g_dbg), "cam=%llX d=%.1f", (unsigned long long)best, best_d);
    return best;
}

/* World/dictionary. */
/* The authoritative roster is WorldEntitiesModel._battleCharacters (+0x18).
 * Scanning for character objects also finds the pre-allocated "not spawned"
 * set, which reads as markers on people who are not in the world. */

static int dict_looks_valid(uint64_t dict) {
    if (!PL(dict)) return 0;
    uint64_t k = u_klass(dict);
    if (!PL(k)) return 0;
    char nm[64];
    if (!u_class_name(k, nm, sizeof(nm))) return 0;
    return strstr(nm, "Dictionary") != NULL;
}

static int probe_world_class(uint64_t klass) {
    static uint8_t buf[0x10000];
    for (int r = 0; r < g_nregions; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != klass) continue;
                if (dict_looks_valid(rr_u64(a + i + 0x18))) return 1;
            }
        }
    }
    return 0;
}

static uint64_t resolve_world_class(void) {
    uint64_t api = resolve_class_api("WorldEntitiesModel");
    if (api) return api;
    collect_hits("WorldEntitiesModel");
    if (!g_nhits) return 0;
    collect_candidates("WorldEntitiesModel");
    for (int i = 0; i < g_ncands; i++)
        if (probe_world_class(g_cands[i])) return g_cands[i];
    return 0;
}

/* Scan one address range for a world instance. Split out so the hinted pass
 * below reuses exactly the same validation rather than keeping a second copy. */
static uint64_t world_scan_range(uint64_t lo, uint64_t hi) {
    if (!g_k_world || hi <= lo) return 0;
    uint8_t buf[0x10000];
    uint64_t best = 0;
    LONG best_n = -1;
    int best_local = 0;
    for (int r = 0; r < g_nregions; r++) {
        uint64_t rlo = g_regions[r].base, rhi = rlo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        if (rhi <= lo || rlo >= hi) continue;
        uint64_t a0 = (rlo > lo ? rlo : lo) & ~(uint64_t)7;
        uint64_t a1 = rhi < hi ? rhi : hi;
        for (uint64_t a = a0; a < a1; a += sizeof(buf)) {
            size_t sz = (size_t)((a1 - a) < sizeof(buf) ? (a1 - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != g_k_world) continue;
                uint64_t w = a + i;
                uint64_t dict = rr_u64(w + RR_OFF_WEM_BATTLE_CHARS);
                if (!dict_looks_valid(dict)) continue;
                int n = 0, stride = 0, live = 0, has_local = 0;
                uint64_t entries = 0;
                if (!u_dict(dict, &n, &entries, &stride) || n > 4096 || stride != 0x18) continue;
                for (int k = 0; k < n; ++k) {
                    uint64_t obj = rr_u64(entries + (uint64_t)k * stride + 0x10);
                    if (!PL(obj) || rr_u64(obj) != g_k_char) continue;
                    ++live;
                    if (PL(rr_u64(obj + RR_OFF_CHAR_PBM))) has_local = 1;
                }
                if (has_local > best_local || (has_local == best_local && live > best_n)) {
                    best_local = has_local; best_n = live; best = w;
                }
            }
        }
    }
    return best_local ? best : 0;
}

/* Choose a validated world candidate using its roster size.
 * Search near the previous address first because scene replacements may be
 * allocated nearby; fall back to committed regions if the local search fails.
 * This is a discovery heuristic, not proof that a candidate is active.
 */
static uint64_t find_world_instance(void) {
    if (!g_k_world) return 0;
    /* Only trust the saved address when it came from this same process: after
     * a game restart it is a stale pointer into a dead heap and the window
     * sweep would be wasted work. */
    uint64_t hint = PL(g_world) ? g_world
                                : (g_saved_pid == g_rr_pid ? g_saved_world : 0);
    if (PL(hint)) {
        const uint64_t window = 0x08000000ull;   /* 128 MB either side */
        uint64_t lo = hint > window ? hint - window : 0;
        uint64_t found = world_scan_range(lo, hint + window);
        if (found) return found;
    }
    uint64_t lo = ~(uint64_t)0, hi = 0;
    for (int r = 0; r < g_nregions; r++) {
        if (g_regions[r].size < 0x1000) continue;
        if (g_regions[r].base < lo) lo = g_regions[r].base;
        uint64_t end = g_regions[r].base + g_regions[r].size;
        if (end > hi) hi = end;
    }
    return hi > lo ? world_scan_range(lo, hi) : 0;
}

/* Fill the cache from the roster dictionary. */
/* Battle scene entities. */
/* BattleEntitiesModel is the scene-level container: it holds the entity map at
 * +0x50, a reference to the world entities model at +0x60 and the local player
 * at +0xD0. Reading that map avoids depending on which WorldEntitiesModel
 * instance happens to be current. */
static uint64_t g_k_battle = 0;

static int views_updater_valid(uint64_t updater, uint64_t battle) {
    if (!PL(updater) || !PL(battle) || rr_u64(updater + RR_OFF_VIEWS_BATTLE) != battle ||
        !class_named(rr_u64(updater), "BattleEntitiesUpdater")) return 0;
    uint64_t model=rr_u64(updater+RR_OFF_VIEWS_MODEL);
    if (!PL(model) || !class_named(rr_u64(model), "BattleModel") || rr_u64(model+RR_OFF_BATTLE_ENTITIES)!=battle) return 0;
    return 1;
}

/* Live BattleCharacterEntityView objects, read from the same dictionary the
 * roster walk uses. In-game chams needs the managed view pointers, and Unity's
 * scene queries (Object.FindObjectsByType) are main-thread only: calling them
 * from an injected worker faults inside the scene graph. This is memory reads
 * against the game's own container, so it is safe from any thread. */
int rr_character_views(uint64_t* out, int capacity) {
    if (!out || capacity <= 0) return 0;
    uint64_t updater = g_views_updater, battle = g_battle;
    if (!views_updater_valid(updater, battle) || rr_u64(battle + RR_OFF_BATTLE_WORLD) != g_world)
        return 0;
    uint64_t dict = rr_u64(updater + RR_OFF_VIEWS_CHARACTERS), entries = 0;
    int n = 0, stride = 0;
    if (!u_dict(dict, &n, &entries, &stride) || n > 4096 || stride != 0x18) return 0;
    int count = 0;
    for (int i = 0; i < n && count < capacity; ++i) {
        uint8_t e[0x18]; uint64_t view;
        if (!rr_read(entries + (uint64_t)i * 0x18, e, sizeof(e))) continue;
        memcpy(&view, e + 0x10, 8);
        if (!PL(view) || !class_named(rr_u64(view), "BattleCharacterEntityView")) continue;
        int seen = 0;
        for (int k = 0; k < count; ++k) if (out[k] == view) { seen = 1; break; }
        if (!seen) out[count++] = view;
    }
    return count;
}

typedef struct { uint64_t needle, world, result; int updater; volatile LONG found; } SCENE_SEARCH;
typedef struct { SCENE_SEARCH* shared; uint64_t lo, hi; } SCENE_RANGE;
static DWORD WINAPI scene_search_thread(LPVOID arg) {
    SCENE_RANGE* range=(SCENE_RANGE*)arg; SCENE_SEARCH* q=range->shared;
    uint8_t buf[0x10000];
    for (uint64_t a=range->lo; a<range->hi; a+=sizeof(buf)) {
        if (InterlockedCompareExchange(&q->found,0,0) || q->world!=g_world) break;
        size_t n=(size_t)((range->hi-a)<sizeof(buf)?range->hi-a:sizeof(buf));
        if (!rr_read(a,buf,n)) continue;
        for (size_t i=0;i+8<=n;i+=8) {
            uint64_t value; memcpy(&value,buf+i,8);
            uint64_t off=q->updater==2?RR_OFF_AIM_BATTLE:
                (q->updater==3?RR_OFF_VIEWS_MODEL:(q->updater?RR_OFF_VIEWS_BATTLE:RR_OFF_BATTLE_WORLD));
            if (value!=q->needle || a+i<off) continue;
            uint64_t obj=a+i-off;
            int valid=q->updater==2 ? (class_named(rr_u64(obj),"BattleAimTargetUpdater") &&
                                      class_named(rr_u64(rr_u64(obj+RR_OFF_AIM_BRAIN)),"CinemachineBrain"))
                : q->updater ? (views_updater_valid(obj,g_battle) && PL(rr_u64(obj+RR_OFF_VIEWS_CHARACTERS)))
                : (class_named(rr_u64(obj),"BattleEntitiesModel") && PL(rr_u64(obj+RR_OFF_BATTLE_SLOTS)));
            if (valid && q->world==g_world && InterlockedCompareExchange(&q->found,1,0)==0) { q->result=obj; return 0; }
        }
    }
    return 0;
}
static uint64_t scene_search(const BATTLE_SCAN* scan, uint64_t needle, int updater) {
    if (!PL(needle)) return 0;
    for (int pass=0;pass<2;++pass) for (int r=0;r<scan->nregions;++r) {
        uint64_t lo=scan->regions[r].base, hi=lo+scan->regions[r].size;
        uint64_t nearlo=needle>0x40000000ULL?needle-0x40000000ULL:0, nearhi=needle+0x40000000ULL;
        if (pass==0) { if (lo<nearlo) lo=nearlo; if (hi>nearhi) hi=nearhi; }
        else if (lo>=nearlo && hi<=nearhi) continue;
        if (hi<=lo || hi-lo<8) continue;
        SCENE_SEARCH q={0}; q.needle=needle; q.world=scan->world; q.updater=updater;
        if (hi-lo < 0x100000) {
            SCENE_RANGE one={&q,lo,hi}; scene_search_thread(&one);
            if (q.result) return q.result;
            if (scan->world!=g_world) return 0;
            continue;
        }
        SCENE_RANGE ranges[SCAN_THREADS]; HANDLE threads[SCAN_THREADS]={0};
        uint64_t words=(hi-lo)/8;
        for (int i=0;i<SCAN_THREADS;++i) {
            ranges[i].shared=&q; ranges[i].lo=lo+(words*(uint64_t)i/SCAN_THREADS)*8;
            ranges[i].hi=lo+(words*(uint64_t)(i+1)/SCAN_THREADS)*8;
            threads[i]=rr_thread_start(scene_search_thread,&ranges[i]);
            if (!threads[i]) scene_search_thread(&ranges[i]);
        }
        for (int i=0;i<SCAN_THREADS;++i) if (threads[i]) { WaitForSingleObject(threads[i],INFINITE); CloseHandle(threads[i]); }
        if (q.result) return q.result;
        if (scan->world!=g_world) return 0;
    }
    return 0;
}
static uint64_t find_battle_for_world(const BATTLE_SCAN* scan) { return scene_search(scan,scan->world,0); }
static uint64_t find_views_updater(const BATTLE_SCAN* scan, uint64_t battle) {
    const uint64_t model = g_battle_model;
    if (u_is(model, "BattleModel") && rr_u64(model + RR_OFF_BATTLE_ENTITIES) == battle) {
        const uint64_t views = scene_search(scan, model, 3);
        if (views) return views;
    }
    return scene_search(scan, battle, 1);
}

static DWORD WINAPI battle_scan_worker(LPVOID p) {
    BATTLE_SCAN* scan = (BATTLE_SCAN*)p;
    uint64_t battle = scan->battle;
    if (!PL(battle) || rr_u64(battle + RR_OFF_BATTLE_WORLD) != scan->world)
        battle = find_battle_for_world(scan);
    if (battle && scan->world == g_world) {
        InterlockedExchange64((volatile LONG64*)&g_battle, (LONG64)battle);
        uint64_t updater = g_views_updater;
        if (!views_updater_valid(updater, battle)) updater = find_views_updater(scan, battle);
        if (updater && scan->world == g_world && battle == g_battle) {
            const uint64_t model = rr_u64(updater + RR_OFF_VIEWS_MODEL);
            if (u_is(model, "BattleModel") && rr_u64(model + RR_OFF_BATTLE_ENTITIES) == battle)
                InterlockedExchange64((volatile LONG64*)&g_battle_model, (LONG64)model);
        }
        if (updater && updater != g_views_updater && scan->world == g_world && battle == g_battle) {
            InterlockedExchange64((volatile LONG64*)&g_views_updater, (LONG64)updater);
            if (g_views_ready_event) SetEvent(g_views_ready_event);
        }
        uint64_t aim = updater && !PL(g_aim_updater) ? scene_search(scan, battle, 2) : 0;
        if (aim && scan->world == g_world && battle == g_battle)
            InterlockedExchange64((volatile LONG64*)&g_aim_updater, (LONG64)aim);
    }
    free(scan);
    InterlockedExchange(&g_battle_scan_busy, 0);
    return 0;
}

/* Match both entity IDs before using the roster identity. */
static void cache_roster_identity(ENT_CACHE* stage, int count) {
    uint64_t battle = g_battle;
    if (!PL(battle) || rr_u64(battle + RR_OFF_BATTLE_WORLD) != g_world) return;
    uint64_t slots = rr_u64(battle + RR_OFF_BATTLE_SLOTS);
    uint64_t dict = PL(slots) ? rr_u64(slots + 0x18) : 0;
    int n = 0, stride = 0; uint64_t entries = 0;
    if (!u_dict(dict, &n, &entries, &stride) || n > 4096 || stride != 0x18) return;
    for (int i = 0; i < n; ++i) {
        uint8_t e[0x18];
        if (!rr_read(entries + (uint64_t)i * 0x18, e, sizeof(e))) continue;
        int32_t key; uint64_t slot;
        memcpy(&key, e + 8, 4); memcpy(&slot, e + 0x10, 8);
        if (!PL(slot)) continue;
        int j; for (j = 0; j < count && stage[j].id != key; ++j) {}
        if (j == count) continue;
        uint64_t data = rr_u64(slot + 0x10);
        uint64_t idref = PL(data) ? rr_u64(data + 0x20) : 0;
        int32_t id = -1;
        if (!PL(idref) || !rr_read(idref + 0x18, &id, sizeof(id)) || id != key) continue;
        uint64_t ref = rr_u64(data + 0x30); /* BattleCharacterSlotData.Nickname */
        uint64_t str = PL(ref) ? rr_u64(ref + RR_OFF_REF_VALUE) : 0;
        if (PL(str)) u_string(str, stage[j].name, sizeof(stage[j].name));
        ref = rr_u64(data + 0x38); /* BattleCharacterSlotData.ClanTag */
        str = PL(ref) ? rr_u64(ref + RR_OFF_REF_VALUE) : 0;
        if (PL(str)) u_string(str, stage[j].clan_tag, sizeof(stage[j].clan_tag));
    }
}

/* Seated labels and bounds both need the rendered pose while the car moves. */
static int needs_mesh_pose(const BR_Config* cfg, int seated) {
    if (cfg->enabled && br_local_esp_enabled(cfg)) return 1;
#ifdef RR_INTERNAL
    if (seated && cfg->enabled) return 1;
#else
    (void)seated;
#endif
    return cfg->show_box || cfg->show_skeleton || cfg->cham_on || cfg->aa_on || cfg->show_tracers;
}

/* Rig discovery counters. One shot per session, written next to the internal
 * log by probe_rig_log() once a roster exists, so a build that maps nothing
 * leaves evidence instead of a silent empty box. */
static int g_rig_seen, g_rig_class, g_rig_anchors, g_rig_root, g_rig_mapped;
static int g_cham_ok, g_cham_fail, g_rig_logged, g_rig_skel, g_rig_tr;

static void probe_rig_log(int count) {
    static int passes;
    if (g_rig_logged || count <= 0) return;
    /* Give the dictionary a few passes to settle before judging it; a mapped
     * rig on any pass is the success case and logs immediately. */
    ++passes;
    if (!g_rig_mapped && passes < 8) return;
    g_rig_logged = 1;
    char path[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, (DWORD)sizeof(path));
    if (n > 0 && n + 32 < sizeof(path)) strcat_s(path, sizeof(path), "\\RobRunInternal\\rig-probe.log");
    FILE* f = path[0] ? fopen(path, "w") : NULL;
    if (!f) return;
    fprintf(f, "roster=%d views=%d class_ok=%d anchors_ok=%d root_ok=%d tr_ok=%d mapped=%d cham_ok=%d cham_fail=%d "
               "skel=%d updater=%016llX battle=%016llX world=%016llX\n",
            count, g_rig_seen, g_rig_class, g_rig_anchors, g_rig_root, g_rig_tr, g_rig_mapped,
            g_cham_ok, g_cham_fail, g_rig_skel, (unsigned long long)g_views_updater,
            (unsigned long long)g_battle, (unsigned long long)g_world);
    fclose(f);
}

/* CharacterMovementView (+0x20 on the entity view) owns the character's root
 * Transform (+0x20). A native Transform carries its hierarchy (+0x28) and slot
 * (+0x30), so the whole rig can be located from that one transform; the five
 * head/torso anchors are only a faster seed for the same hierarchy. When those
 * components are missing or re-laid-out, this keeps the rig resolvable. */
static int mesh_from_native_transform(ENT_CACHE* e, uint64_t component) {
    uint64_t native = PL(component) ? rr_u64(component + 0x10) : 0;
    if (!PL(native)) return 0;
    uint8_t fields[12];
    if (!rr_read(native + 0x28, fields, sizeof(fields))) return 0;
    uint64_t h = 0; int idx = -1;
    memcpy(&h, fields, 8); memcpy(&idx, fields + 8, 4);
    if (!PL(h) || idx < 0) return 0;
    int capacity = rr_i32(h + 0x10);
    uint64_t pointers = h ? rr_u64(h + 0x30) : 0;
    if (capacity < 1 || capacity > 2048 || idx >= capacity || !PL(pointers) ||
        rr_u64(pointers + (uint64_t)idx * 8) != native) return 0;
    e->mesh_h = h;
    e->mesh_idx[0] = idx; e->mesh_native[0] = native;
    for (int k = 1; k < 5; ++k) { e->mesh_idx[k] = -1; e->mesh_native[k] = 0; }
    e->mesh_root = 1;
    e->mesh_view_at = GetTickCount();
    return 1;
}

static int mesh_from_root_transform(ENT_CACHE* e, uint64_t view) {
    uint64_t movement = rr_u64(view + 0x20);
    return PL(movement) ? mesh_from_native_transform(e, rr_u64(movement + 0x20)) : 0;
}

/* Link each roster ID to its character view and the four animated transforms
 * explicitly exposed by the head and torso view components. */
static void cache_mesh_views(ENT_CACHE* stage, int count) {
    static ENT_CACHE previous[RR_MAX_PLAYERS];
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;
    int previous_count=0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    previous_count=g_ncache;
    if (previous_count>RR_MAX_PLAYERS) previous_count=RR_MAX_PLAYERS;
    if (previous_count>0) memcpy(previous,g_cache,(size_t)previous_count*sizeof(previous[0]));
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    uint64_t updater = g_views_updater, battle = g_battle;
    if (!views_updater_valid(updater, battle) || rr_u64(battle + RR_OFF_BATTLE_WORLD) != g_world) goto retain;
    uint64_t dict = rr_u64(updater + RR_OFF_VIEWS_CHARACTERS), entries = 0;
    int n = 0, stride = 0;
    if (!u_dict(dict, &n, &entries, &stride) || n > 4096 || stride != 0x18) goto retain;
    for (int i = 0; i < n; ++i) {
        uint8_t e[0x18]; int32_t key; uint64_t view;
        if (!rr_read(entries + (uint64_t)i * 0x18, e, sizeof(e))) continue;
        memcpy(&key, e + 8, 4); memcpy(&view, e + 0x10, 8);
        if (!PL(view)) continue;
        ++g_rig_seen;
        int j; for (j = 0; j < count && stage[j].id != key; ++j) {}
        if (j == count || !class_named(rr_u64(view), "BattleCharacterEntityView")) continue;
        ++g_rig_class;
        uint64_t anim = rr_u64(view + 0x28);
        uint64_t head = PL(anim) ? rr_u64(anim + 0x28) : 0;
        uint64_t torso = PL(anim) ? rr_u64(anim + 0x30) : 0;
        int ok = 0;
        uint64_t hierarchy = 0, natives[5]; int indices[5];
        if (PL(head) && PL(torso)) {
            uint64_t transforms[5] = { rr_u64(head + 0x28), rr_u64(head + 0x20),
                                       rr_u64(torso + 0x30), rr_u64(torso + 0x28), rr_u64(torso + 0x20) };
            ok = 1;
            for (int k = 0; k < 5; ++k) {
                uint64_t native = PL(transforms[k]) ? rr_u64(transforms[k] + 0x10) : 0;
                uint64_t h = PL(native) ? rr_u64(native + 0x28) : 0;
                int idx = PL(native) ? rr_i32(native + 0x30) : -1;
                int capacity=PL(h)?rr_i32(h+0x10):0;
                uint64_t pointers=PL(h)?rr_u64(h+0x30):0;
                if (!PL(h) || (k && h != hierarchy) || capacity<1 || capacity>2048 || idx<0 || idx>=capacity ||
                    !PL(pointers) || rr_u64(pointers+(uint64_t)idx*8)!=native) { ok = 0; break; }
                hierarchy = h; indices[k] = idx; natives[k]=native;
            }
        }
        if (ok) {
            ++g_rig_anchors;
            stage[j].mesh_h = hierarchy; memcpy(stage[j].mesh_idx, indices, sizeof(indices));
            memcpy(stage[j].mesh_native,natives,sizeof(natives));
            stage[j].mesh_view_at = GetTickCount();
        } else if (mesh_from_root_transform(&stage[j], view)) {
            /* The movement transform always exists; the head/torso views are
             * optional components whose layout can change between builds. */
            ++g_rig_root;
            ok = 1;
        }
        if (ok) {
            ++g_rig_mapped;
            const BR_Config* config = br_get();
            if (needs_mesh_pose(config, stage[j].inveh) &&
                (!stage[j].local || config->show_self || br_local_esp_enabled(config) || config->show_tracers)) {
                const ENT_CACHE* old=NULL;
                for (int q=0;q<previous_count;++q)
                    if (previous[q].obj==stage[j].obj && previous[q].mesh_h==stage[j].mesh_h && previous[q].cham_ready) { old=&previous[q]; break; }
                cache_cham_rig(&stage[j],old);
            }
        }
    }
    /* Last resort before retention: the character's own Transform component
     * (the one the roster reads position from) is present for every spawned
     * character, so the rig can still resolve when the views dictionary is
     * unavailable for this build or an entry is missing from it. */
    for (int j=0; j<count; ++j) {
        if (stage[j].mesh_h || stage[j].hidden_in_object || !PL(stage[j].tr)) continue;
        if (!mesh_from_native_transform(&stage[j], stage[j].tr)) continue;
        ++g_rig_tr; ++g_rig_mapped;
        const BR_Config* config = br_get();
        if (needs_mesh_pose(config, stage[j].inveh) && (!stage[j].local || config->show_self || br_local_esp_enabled(config) || config->show_tracers))
            cache_cham_rig(&stage[j], NULL);
    }
retain:
    /* A partial dictionary/rig refresh should not make a rendered character
     * jump back to its 20 Hz network position. Retain only a recent mapping
     * for the same object and verify all native slots before reusing it. */
    for (int j=0; j<count; ++j) {
        if (stage[j].mesh_h && stage[j].cham_ready) continue;
        for (int q=0; q<previous_count; ++q) {
            const ENT_CACHE* old=&previous[q];
            if (old->obj!=stage[j].obj || old->id!=stage[j].id ||
                !old->mesh_view_at || GetTickCount()-old->mesh_view_at>1500 || !PL(old->mesh_h)) continue;
            if (stage[j].mesh_h && (stage[j].mesh_h!=old->mesh_h ||
                stage[j].mesh_native[0]!=old->mesh_native[0])) break;
            int capacity=rr_i32(old->mesh_h+0x10);
            uint64_t slots=rr_u64(old->mesh_h+0x30);
            if (capacity<1 || capacity>2048 || !PL(slots)) break;
            int valid=1, checked=0;
            for (int k=0; k<5; ++k) {
                int idx=old->mesh_idx[k];
                if (idx<0) continue;          /* rig discovered from the root transform */
                if (idx>=capacity || !PL(old->mesh_native[k]) ||
                    rr_u64(slots+(uint64_t)idx*8)!=old->mesh_native[k]) { valid=0; break; }
                ++checked;
            }
            if (checked && !PL(old->mesh_native[0])) valid=0;
            if (!valid) break;
            if (!stage[j].mesh_h) {
                stage[j].mesh_h=old->mesh_h;
                memcpy(stage[j].mesh_idx,old->mesh_idx,sizeof(old->mesh_idx));
                memcpy(stage[j].mesh_native,old->mesh_native,sizeof(old->mesh_native));
                stage[j].mesh_root=old->mesh_root;
                stage[j].mesh_view_at=old->mesh_view_at;
            }
            if (old->cham_ready) {
                /* The five torso anchors cannot validate the other 89 joints.
                 * Recheck the complete palette before retaining a full rig. */
                ENT_CACHE validated=*old;
                validated.cham_ready=0;
                cache_cham_rig(&validated,old);
                if (validated.cham_ready) {
                    memcpy(stage[j].cham_idx,validated.cham_idx,sizeof(validated.cham_idx));
                    memcpy(stage[j].cham_native,validated.cham_native,sizeof(validated.cham_native));
                    stage[j].mesh_first=validated.mesh_first; stage[j].mesh_span=validated.mesh_span;
                    stage[j].cham_ready=1;
                }
            }
            break;
        }
    }
    g_rig_skel=0;
    for (int j=0; j<count; ++j) if (PL(stage[j].skel)) ++g_rig_skel;
    probe_rig_log(count);
}

static int probe_battle_class(uint64_t klass) {
    static uint8_t buf[0x10000];
    for (int r = 0; r < g_nregions; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != klass) continue;
                uint64_t obj = a + i;
                if (dict_looks_valid(rr_u64(obj + RR_OFF_BATTLE_MODELS))) return 1;
                uint64_t w = rr_u64(obj + RR_OFF_BATTLE_WORLD);
                if (PL(w) && dict_looks_valid(rr_u64(w + RR_OFF_WEM_BATTLE_CHARS))) return 1;
            }
        }
    }
    return 0;
}

static uint64_t resolve_battle_class(void) {
    collect_hits("BattleEntitiesModel");
    if (!g_nhits) return 0;
    collect_candidates("BattleEntitiesModel");
    for (int i = 0; i < g_ncands; i++)
        if (probe_battle_class(g_cands[i])) return g_cands[i];
    return 0;
}

static uint64_t find_battle_instance(void) {
    if (!g_k_battle) return 0;
    uint64_t best = 0;
    LONG best_n = -1;
    static uint8_t buf[0x10000];
    for (int r = 0; r < g_nregions; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != g_k_battle) continue;
                uint64_t b = a + i;
                uint64_t dict = rr_u64(b + RR_OFF_BATTLE_MODELS);
                if (!dict_looks_valid(dict)) continue;
                LONG n = rr_i32(dict + 0x20);
                if (n > best_n) { best_n = n; best = b; }
            }
        }
    }
    return best;
}

/* Fill from BattleEntitiesModel._models (+0x50). */
static int cache_from_battle(void) {
    const uint64_t battle = g_battle;
    if (!PL(battle) || !PL(g_world) || rr_u64(battle + RR_OFF_BATTLE_WORLD) != g_world) {
        InterlockedExchange(&g_diag_bf, 1);
        return 0;
    }
    uint64_t dict = rr_u64(battle + RR_OFF_BATTLE_MODELS);
    int n = 0, stride = 0;
    uint64_t entries = 0;
    if (!u_dict(dict, &n, &entries, &stride)) { InterlockedExchange(&g_diag_bf, 2); return 0; }
    if (n <= 0 || n > 4096 || stride < 0x18) { InterlockedExchange(&g_diag_bf, 2); return 0; }

    static uint8_t buf[0x10000];
    size_t bytes = (size_t)n * (size_t)stride;
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    int per = (int)(bytes / (size_t)stride);
    if (per <= 0) { InterlockedExchange(&g_diag_bf, 2); return 0; }
    if (!rr_read(entries, buf, bytes)) { InterlockedExchange(&g_diag_bf, 3); return 0; }

    int count = 0;
    for (int i = 0; i < per && count < RR_MAX_PLAYERS; i++) {
        uint64_t obj = 0;
        memcpy(&obj, buf + (size_t)i * (size_t)stride + 0x10, 8);
        if (!PL(obj)) continue;
        if (rr_u64(obj) != g_k_char) continue;      /* only characters */
        ENT_CACHE tmp;
        fill_entry(&tmp, obj, world_gen());
        if (!tmp.obj) continue;
        g_stage[count++] = tmp;
    }
    InterlockedExchange(&g_diag_bf, count ? 7 : 4);
    if (count == 0) return 0;

    cache_roster_identity(g_stage, count);
    cache_mesh_views(g_stage, count);
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_cache, g_stage, sizeof(ENT_CACHE) * (size_t)count);
    g_ncache = count;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    g_anchor = g_cache[0].obj;
    return count;
}

static int cache_from_world(void) {
    if (!PL(g_world) || !g_k_world) { InterlockedExchange(&g_diag_wf, 1); return -1; }
    uint64_t klass = 0, dict = 0;
    if (!rr_read(g_world, &klass, sizeof(klass))) return -2;
    if (klass != g_k_world) { InterlockedExchange(&g_diag_wf, 1); return -1; }
    if (!rr_read(g_world + RR_OFF_WEM_BATTLE_CHARS, &dict, sizeof(dict))) return -2;
    /* The game can tear down a world without recycling its class word. A
     * successfully read null roster is not a transient read failure. */
    if (!PL(dict)) { InterlockedExchange(&g_diag_wf, 2); return -1; }
    uint64_t dict_class = 0;
    if (!rr_read(dict, &dict_class, sizeof(dict_class))) return -2;
    if (!PL(dict_class)) { InterlockedExchange(&g_diag_wf, 3); return -1; }
    char dict_name[64];
    if (!u_class_name(dict_class, dict_name, sizeof(dict_name))) return -2;
    if (!strstr(dict_name, "Dictionary")) { InterlockedExchange(&g_diag_wf, 3); return -1; }
    int used = 0;
    if (!rr_read(dict + U_DICT_COUNT, &used, sizeof(used)) || used < 0 || used > 4096) return -2;
    if (used == 0) {
        InterlockedExchange(&g_diag_wf, 4);
        if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
        g_ncache = 0;
        if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
        return 0;
    }
    int n = 0, stride = 0;
    uint64_t entries = 0;
    if (!u_dict(dict, &n, &entries, &stride)) { InterlockedExchange(&g_diag_wf, 5); return -2; }
    if (n <= 0 || n > 4096 || stride < 0x18) { InterlockedExchange(&g_diag_wf, 5); return -2; }

    static uint8_t buf[0x10000];
    size_t bytes = (size_t)n * (size_t)stride;
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    int per = (int)(bytes / (size_t)stride);
    if (per <= 0) { InterlockedExchange(&g_diag_wf, 5); return -2; }
    if (!rr_read(entries, buf, bytes)) { InterlockedExchange(&g_diag_wf, 6); return -2; }

    int count = 0;
    for (int i = 0; i < per && count < RR_MAX_PLAYERS; i++) {
        uint64_t obj = 0;
        memcpy(&obj, buf + (size_t)i * (size_t)stride + 0x10, 8);
        if (!PL(obj)) continue;
        ENT_CACHE tmp;
        fill_entry(&tmp, obj, world_gen());
        if (!tmp.obj) continue;
        g_stage[count++] = tmp;
    }
    InterlockedExchange(&g_diag_wf, count ? 7 : 6);

    cache_roster_identity(g_stage, count);
    cache_mesh_views(g_stage, count);
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_cache, g_stage, sizeof(ENT_CACHE) * (size_t)count);
    g_ncache = count;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    if (count) g_anchor = g_cache[0].obj;
    return count;
}

/* Read world collections independently. Each position is validated against its
 * managed type so a stale world cannot turn arbitrary memory into a marker. */
static void cache_world_markers(void) {
    static const struct { unsigned dict_off, pos_off; int kind; const char *klass, *name; } sources[] = {
        {RR_OFF_WEM_CONTAINERS, RR_OFF_CONTAINER_POS, 3, "ContainerModel", "Container"},
        {RR_OFF_WEM_DOORS, RR_OFF_DOOR_POS, 4, "DoorModel", "Door"},
        {RR_OFF_WEM_DROPPED_LOOT, RR_OFF_DROPPED_LOOT_POS, 5, "LootModel", "Dropped loot"},
        {RR_OFF_WEM_VEHICLE_LOOT, RR_OFF_VEHICLE_LOOT_POS, 6, "VehicleLootModel", "Vehicle loot"}
    };
    LOOT_CACHE staged[RR_MAX_WORLD_MARKERS];
    float distance2[RR_MAX_WORLD_MARKERS]; RR_V3 origin = {0, 0, 0};
    int total = 0; uint64_t world = g_world;
    if (!PL(world) || rr_u64(world) != g_k_world) return;
    uint64_t camera = g_cam_obj;
    int have_origin = camera_validate(camera) && rr_read(camera + RR_OFF_CAM_POS, &origin, sizeof origin) &&
        isfinite(origin.x) && isfinite(origin.y) && isfinite(origin.z);
    for (int s = 0; s < 4; ++s) {
        BR_Config* cfg = br_get();
        int enabled = s == 0 ? cfg->show_containers : s == 1 ? cfg->show_doors : s == 2 ? cfg->show_dropped_loot : cfg->show_vehicle_loot;
        if (!enabled) continue;
        uint64_t dict = rr_u64(world + sources[s].dict_off), entries = 0;
        int n = 0, stride = 0;
        if (!u_dict(dict, &n, &entries, &stride) || n < 0 || n > 4096 || stride != 0x18) continue;
        int category_start = total;
        for (int i = 0; i < n; ++i) {
            uint8_t row[0x18]; uint64_t obj; int32_t id;
            if (!rr_read(entries + (uint64_t)i * 0x18, row, sizeof row)) break;
            memcpy(&id, row + 8, 4); memcpy(&obj, row + 0x10, 8);
            if (!u_is(obj, sources[s].klass)) continue;
            uint64_t pos = rr_u64(obj + sources[s].pos_off);
            unsigned position_offset;
            if (u_is(pos, "PositionModel")) position_offset = RR_OFF_POS_X;
            else if (u_is(pos, "InteractPositionModel")) position_offset = RR_OFF_INTERACT_POS_X;
            else continue;
            RR_V3 v;
            if (!rr_read(pos + position_offset, &v, sizeof v) || !isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z) ||
                fabsf(v.x) > 500000 || fabsf(v.y) > 500000 || fabsf(v.z) > 500000) continue;
            float dx = v.x - origin.x, dy = v.y - origin.y, dz = v.z - origin.z;
            float score = dx * dx + dy * dy + dz * dz;
            int at = rr_world_marker_reserve(distance2, category_start, &total,
                                             RR_MAX_WORLD_MARKERS / 4, score, have_origin);
            if (at < 0) continue;
            LOOT_CACHE* m = &staged[at]; memset(m, 0, sizeof *m);
            m->id = id; m->kind = sources[s].kind; m->pos = v; m->obj = obj;
            snprintf(m->name, sizeof m->name, "%s", sources[s].name);
            if (sources[s].kind == 4) {
                uint64_t state = rr_u64(obj + RR_OFF_DOOR_STATE);
                uint64_t data = u_is(state, "DoorStateModel") ? rr_u64(state + RR_OFF_DOOR_STATE_DATA) : 0;
                uint64_t value = u_is(data, "ReferenceData`1") ? rr_u64(data + RR_OFF_PRIM_VALUE) : 0;
                char name[24] = {0};
                if (u_is(value, "DoorStateDescription") && u_string(rr_u64(value + 0x10), name, sizeof name))
                    snprintf(m->name, sizeof m->name, "Door: %s", name);
            }
        }
    }
    if (world != g_world || rr_u64(world) != g_k_world) return;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_world_markers, staged, sizeof(staged[0]) * (size_t)total);
    g_nworld_markers = total; g_markers_world = world; g_markers_stamp = GetTickCount();
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
}

/* World membership defines hiding-object coverage. Cosmetic type delegates
 * and compiler-generated closure names must not exclude boxes or toilets. */
static void cache_hide_objects(void) {
    uint64_t world = g_world;
    static RR_HIDE_SNAPSHOT staged;
    if (!PL(world) || rr_u64(world) != g_k_world) return;
    if(!rr_hide_read(g_battle_model,world,&staged))memset(&staged,0,sizeof(staged));
    if (world != g_world || rr_u64(world) != g_k_world) return;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    g_hide_snapshot=staged;
    memcpy(g_hide_objects,staged.objects,sizeof(staged.objects[0])*(size_t)staged.nobjects);
    g_nhide_objects=staged.nobjects;g_hide_world=world;g_hide_stamp=GetTickCount();
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
}

static uint64_t loot_delegate_value(uint64_t func, int direct_offset, const char* closure) {
    if (!u_is(func,"Func`1")) return 0;
    uint64_t target=rr_u64(func+0x20);
    if (closure && u_is(target,closure)) return rr_u64(target+(uint64_t)direct_offset);
    if (!u_is(target,"<>c__DisplayClass5_0`1")) return 0;
    uint64_t en=rr_u64(target+0x10); int index=rr_i32(target+0x18);
    if (!u_is(en,"Enum`1")) return 0;
    uint64_t list=rr_u64(en+0x10);
    if (!u_is(list,"List`1")) return 0;
    int count=rr_i32(list+0x18); uint64_t array=rr_u64(list+0x10);
    if (!PL(array) || count<0 || count>100000 || index<0 || index>=count ||
        rr_u64(array+0x18)<(uint64_t)count) return 0;
    return rr_u64(array+0x20+(uint64_t)index*8);
}

/* Runs on the discovery worker. A slot is published only after its world,
 * state, description, position and item have all been validated. */
/* Track rejection reasons per loot kind. An invalid chain must remain
 * distinguishable from a successfully read empty/taken flag.
 */
enum {
    LST_SEEN = 0,   /* dictionary entries                                  */
    LST_CLASS,      /* obj is not the expected node class                  */
    LST_DESC,       /* description class wrong, or its key does not match  */
    LST_POS,        /* position unreadable or not finite                   */
    LST_EMPTY,      /* node is already empty/taken - the normal skip        */
    LST_FLAG,       /* the empty/taken flag itself is unreadable            */
    LST_LOOT,       /* loot description class wrong                        */
    LST_INV,        /* item description class wrong                        */
    LST_RARITY,     /* rarity outside 1..6                                 */
    LST_NAME,       /* item name unreadable                                */
    LST_N
};
static int g_loot_stat[2][LST_N];

static void cache_loot(int kind) {
    uint64_t world=g_world;
    int st[LST_N];
    if (!PL(world) || rr_u64(world)!=g_k_world) return;
    LOOT_CACHE staged[RR_MAX_LOOT];
    int total=0;
    uint64_t dict=rr_u64(world+(kind==1?0xA0:0xB0)), entries=0;
    int used=0,n=0,stride=0;
    memset(st,0,sizeof st);
    if (!rr_ok() || (dict && (!PL(dict) || !rr_read(dict+U_DICT_COUNT,&used,4) || used<0 || used>4096))) return;
    if (used && (!u_dict(dict,&n,&entries,&stride) || n!=used || stride!=0x18)) return;
    for (int i=0;i<n && total<RR_MAX_LOOT;++i) {
        uint8_t entry[0x18]; int32_t key; uint64_t obj;
        if (!rr_read(entries+(uint64_t)i*0x18,entry,sizeof(entry))) break;
        st[LST_SEEN]++;
        memcpy(&key,entry+8,4); memcpy(&obj,entry+0x10,8);
        if (!u_is(obj,kind==1?"RandomLootObjectModel":"LootNodeModel")) { st[LST_CLASS]++; continue; }
        uint64_t desc=rr_u64(obj+(kind==1?0x20:0x10)), inventory=0, lootobj=0;
        if (!u_is(desc,kind==1?"RandomLootObjectDescription":"LocationLootNodeDescription") ||
            rr_i32(desc+0x20)!=key) { st[LST_DESC]++; continue; }
        RR_V3 pos;
        uint64_t pos_addr = desc + (kind==1?0x24:0x38);
        if (!rr_read(pos_addr,&pos,sizeof(pos)) ||
            !isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z) ||
            fabsf(pos.x)>500000 || fabsf(pos.y)>500000 || fabsf(pos.z)>500000) { st[LST_POS]++; continue; }
        uint64_t flag_addr = 0;
        if (kind==1) {
            uint64_t state=rr_u64(obj+0x10),ref=rr_u64(obj+0x18); uint8_t taken=255;
            if (!u_is(state,"BoolData") || !rr_read(state+0x18,&taken,1)) { st[LST_FLAG]++; continue; }
            if (taken!=0) { st[LST_EMPTY]++; continue; }
            if (!u_is(ref,"ReferenceData`1")) { st[LST_FLAG]++; continue; }
            flag_addr = state + 0x18;
            uint64_t item=rr_u64(ref+0x18);
            if (!u_is(item,"LootObjectItemDescription")) { st[LST_LOOT]++; continue; }
            lootobj=item;
            inventory=loot_delegate_value(rr_u64(item+0x20),0,NULL);
        } else {
            uint64_t emptyflag=rr_u64(obj+0x30),data=PL(emptyflag)?rr_u64(emptyflag+0x10):0;
            uint8_t value=255;
            if (!u_is(emptyflag,"PrimitiveModel`1") || !u_is(data,"BoolData") ||
                !rr_read(data+0x18,&value,1)) { st[LST_FLAG]++; continue; }
            if (value!=0) { st[LST_EMPTY]++; continue; }
            flag_addr = data + 0x18;
            uint64_t loot=loot_delegate_value(rr_u64(desc+0x30),0x18,"<>c__DisplayClass46_0");
            if (!u_is(loot,"LootNodeLootDescription")) { st[LST_LOOT]++; continue; }
            lootobj=loot;
            inventory=loot_delegate_value(rr_u64(loot+0x30),0x10,"<>c__DisplayClass3_0");
        }
        if (!u_is(inventory,"InventoryItemDescription")) { st[LST_INV]++; continue; }
        int rarity=rr_i32(inventory+0x30);
        if (!rr_ok() || rarity<0 || rarity>6) { st[LST_RARITY]++; continue; }
        LOOT_CACHE *l=&staged[total]; memset(l,0,sizeof(*l));
        if (!u_string(rr_u64(inventory+0x10),l->name,sizeof(l->name))) { st[LST_NAME]++; continue; }
        l->id=key; l->kind=kind; l->rarity=rarity; l->pos=pos;
        l->obj=obj; l->pos_addr=pos_addr; l->flag_addr=flag_addr;
        l->desc=desc; l->loot=lootobj; l->inv=inventory;
        ++total;
    }
    if (world!=g_world || rr_u64(world)!=g_k_world) return;
    int slot=kind-1;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_loot[slot],staged,sizeof(LOOT_CACHE)*(size_t)total);
    g_nloot[slot]=total; g_loot_world[slot]=world; g_loot_stamp[slot]=GetTickCount();
    memcpy(g_loot_stat[slot],st,sizeof st);
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
}

/* Loot vacuum. */

/* Writes to loot state, from the addresses cache_loot already validated.
 *
 *   mode 0 "move"  - write the local player's position over each loot
 *                    position, so proximity pickup sees the item there
 *   mode 1 "taken" - set the taken (loose loot) / empty (node) flag
 *
 * apply == 0 resolves and counts only: that is how to see what would be
 * touched before anything changes.
 *
 * Every entry is re-validated immediately before its write - the model still
 * has the expected class name, and its flag is still 0 - so a cache entry that
 * aged out is skipped instead of writing through a stale address. Writes are
 * capped per call for the same reason.
 *
 * Not re-entrant: one caller at a time (the diagnostic mode or the overlay's
 * worker), which is why the staging buffer below can be static.
 */
int rr_loot_vacuum(int mode, int apply, RR_V3* local_pos_out,
                   int* out_ok, int* out_skipped) {
    static LOOT_CACHE items[RR_MAX_LOOT];
    RR_V3 local_pos;
    int have_local = 0, ok = 0, skipped = 0;
    enum { MAX_WRITES = 64 };

    if (out_ok) *out_ok = 0;
    if (out_skipped) *out_skipped = 0;
    memset(&local_pos, 0, sizeof(local_pos));
    if (local_pos_out) memset(local_pos_out, 0, sizeof(*local_pos_out));
    if (!g_cs_ready) return 0;

    /* The local player's transform is the destination for mode 0. */
    for (int i = 0; i < g_ncache; ++i) {
        RR_V3 p;

        if (!g_cache[i].local || !PL(g_cache[i].tr)) continue;
        if (!rr_read(g_cache[i].tr + RR_OFF_CTM_X, &p, sizeof(p))) continue;
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) continue;
        local_pos = p;
        have_local = 1;
        break;
    }
    if (local_pos_out && have_local) memcpy(local_pos_out, &local_pos, sizeof(local_pos));
    if (mode == 0 && !have_local) return 0;

    for (int kind = 0; kind < 2; ++kind) {
        const char* want = kind == 1 ? "RandomLootObjectModel" : "LootNodeModel";
        int n;

        /* Copy under the lock; the writes below are IOCTLs and must not hold it. */
        EnterCriticalSection(&g_cache_cs);
        n = g_nloot[kind];
        if (n > RR_MAX_LOOT) n = RR_MAX_LOOT;
        memcpy(items, g_loot[kind], sizeof(LOOT_CACHE) * (size_t)n);
        LeaveCriticalSection(&g_cache_cs);

        for (int i = 0; i < n; ++i) {
            const LOOT_CACHE* e = &items[i];
            uint8_t flag = 1;

            if (!PL(e->obj) || !PL(e->flag_addr) || !u_is(e->obj, want)) { skipped++; continue; }
            if (!rr_read(e->flag_addr, &flag, 1) || flag != 0) { skipped++; continue; }

            if (!apply) { ok++; continue; }
            if (ok >= MAX_WRITES) { skipped++; continue; }

            if (mode == 1) {
                uint8_t one = 1;
                if (rr_write(e->flag_addr, &one, 1)) ok++; else skipped++;
            } else {
                if (rr_write(e->pos_addr, &local_pos, sizeof(local_pos))) ok++; else skipped++;
            }
        }
    }

    if (out_ok) *out_ok = ok;
    if (out_skipped) *out_skipped = skipped;
    return ok + skipped;
}

/* Clear addresses and caches tied to the previous process before discovery
 * in a new attachment. Heap addresses from the old process are not reusable
 * even if its replacement has the same executable name.
 */
void game_reattach_reset(void) {
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    g_ncache = 0;
    g_nvehicles = 0;
    g_nloot[0] = g_nloot[1] = 0;
    g_nworld_markers = 0; g_markers_world = 0; g_markers_stamp = 0;
    g_nhide_objects = 0;
    memset(&g_hide_snapshot,0,sizeof(g_hide_snapshot));
    g_hide_world = 0;
    g_hide_stamp = 0;
    g_loot_world[0] = g_loot_world[1] = 0;
    g_loot_stamp[0] = g_loot_stamp[1] = 0;
    g_vehicle_world = 0;
    g_vehicle_generation++;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    g_world = 0;
    g_cam_obj = 0;
    g_k_world = 0;
    g_k_char = 0;
    g_k_cam = 0;
    g_k_battle = 0;
    g_battle = 0;
    g_battle_model = 0;
    g_views_updater = 0;
    g_aim_updater = 0;
    g_saved_pid = 0;
    g_saved_world = 0;
    g_nregions = 0;
    g_nheap = 0;
}

/* Address of the local player's BattleCharacterModel, 0 when the roster is not
 * published yet. Read-only view of the cache under the same lock the scan
 * worker uses. */
uint64_t rr_local_player(void) {
    uint64_t obj = 0;
    int i;
    if (!g_cs_ready) return 0;
    EnterCriticalSection(&g_cache_cs);
    for (i = 0; i < g_ncache; ++i)
        if (g_cache[i].local) { obj = g_cache[i].obj; break; }
    LeaveCriticalSection(&g_cache_cs);
    return obj;
}

int rr_loot_snapshot(RR_LOOT_ENT* out, int max) {    int total = 0, slot;
    if (!out || max <= 0 || !g_cs_ready) return 0;
    for (slot = 0; slot < 2 && total < max; ++slot) {
        int i, n;
        EnterCriticalSection(&g_cache_cs);
        n = g_nloot[slot];
        if (n > RR_MAX_LOOT) n = RR_MAX_LOOT;
        for (i = 0; i < n && total < max; ++i) {
            const LOOT_CACHE* s = &g_loot[slot][i];
            RR_LOOT_ENT* d = &out[total++];
            memset(d, 0, sizeof(*d));
            d->obj = s->obj; d->desc = s->desc;
            d->loot = s->loot; d->inv = s->inv;
            d->pos_addr = s->pos_addr; d->flag_addr = s->flag_addr;
            d->id = s->id; d->kind = s->kind; d->rarity = s->rarity; d->pos = s->pos;
            strncpy(d->name, s->name, sizeof(d->name) - 1);
        }
        LeaveCriticalSection(&g_cache_cs);
    }
    return total;
}

int rr_loot_refresh(RR_LOOT_DEBUG* dbg) {
    uint64_t dict;
    int used;

    if (dbg) {
        memset(dbg, 0, sizeof(*dbg));
        dbg->world = g_world;
        dbg->entries_nodes = -1;
        dbg->entries_loose = -1;
    }
    if (!PL(g_world)) return 0;

    dict = rr_u64(g_world + 0xB0);            /* nodes */
    if (dbg) dbg->dict_nodes = dict;
    if (PL(dict)) {
        used = -1;
        if (rr_read(dict + U_DICT_COUNT, &used, 4) && dbg) dbg->entries_nodes = used;
    }
    dict = rr_u64(g_world + 0xA0);            /* loose loot */
    if (dbg) dbg->dict_loose = dict;
    if (PL(dict)) {
        used = -1;
        if (rr_read(dict + U_DICT_COUNT, &used, 4) && dbg) dbg->entries_loose = used;
    }

    /* Both kinds, because the caller does not know which one it wants and each
     * one writes only its own slot. */
    cache_loot(1);
    cache_loot(2);

    if (dbg) {
        int s;
        for (s = 0; s < 2; ++s) {
            int seen = g_loot_stat[s][LST_SEEN];
            int emp = g_loot_stat[s][LST_EMPTY];
            int bad = seen - g_nloot[s] - emp;
            if (bad < 0) bad = 0;
            if (s == 0) { dbg->loose_seen = seen; dbg->loose_empty = emp; dbg->loose_bad = bad; }
            else        { dbg->nodes_seen = seen; dbg->nodes_empty = emp; dbg->nodes_bad = bad; }
        }
        dbg->cached = g_nloot[0] + g_nloot[1];
    }
    return g_nloot[0] + g_nloot[1];
}

/* Count a vehicle's seat flags only if every entry can be read and validated.
 * A failed read is unknown, not evidence that the vehicle is empty. */
static int vehicle_seat_count(uint64_t seats, int* occupied) {
    if (!PL(seats) || !class_named(rr_u64(seats), "VehicleSeatsModel")) return 0;
    uint64_t map=rr_u64(seats+RR_OFF_SEATS_DATA);
    if (!PL(map) || !class_named(rr_u64(map), "ReferenceMapData`2")) return 0;
    uint64_t list=rr_u64(map+0x18);
    if (!PL(list) || !class_named(rr_u64(list), "List`1")) return 0;
    int n=-1; uint64_t array=rr_u64(list+0x10), length=0;
    if (!rr_read(list+0x18,&n,4) || n<1 || n>32 || !PL(array) ||
        !rr_read(array+0x18,&length,8) || length<(uint64_t)n || length>256) return 0;
    uint64_t items[32];
    if (!rr_read(array+0x20,items,(size_t)n*sizeof(items[0]))) return 0;
    int count=0;
    for (int i=0;i<n;++i) {
        uint64_t seat=items[i];
        if (!PL(seat) || !class_named(rr_u64(seat),"VehicleSeatData")) return 0;
        uint64_t flag=rr_u64(seat+RR_OFF_SEAT_OCCUPIED);
        uint8_t value=255;
        if (!PL(flag) || !class_named(rr_u64(flag),"BoolData") ||
            !rr_read(flag+0x18,&value,1) || value>1) return 0;
        if (value) {
            uint64_t iddata=rr_u64(seat+RR_OFF_SEAT_ENTITY_ID); int32_t id=-1;
            if (!PL(iddata) || !rr_read(iddata+0x18,&id,4) || id<0) return 0;
            ++count;
        }
    }
    *occupied=count; return 1;
}

static void cache_vehicle_render_root(VEH_CACHE* vehicle, uint64_t transform) {
    if (!u_is(transform, "Transform")) return;
    uint64_t native = rr_u64(transform + 0x10);
    uint64_t hierarchy = PL(native) ? rr_u64(native + 0x28) : 0;
    int index = PL(native) ? rr_i32(native + 0x30) : -1;
    int capacity = PL(hierarchy) ? rr_i32(hierarchy + 0x10) : 0;
    if (capacity <= 0 || capacity > 65536 || index < 0 || index >= capacity) return;
    uint64_t arrays[4];
    if (!rr_read(hierarchy + 0x18, arrays, sizeof(arrays)) || !PL(arrays[0]) ||
        !PL(arrays[1]) || !PL(arrays[3]) || rr_u64(arrays[3] + (uint64_t)index * 8) != native) return;
    int depth = 0;
    while (index >= 0 && index < capacity && depth < RR_VEHICLE_POSE_DEPTH) {
        vehicle->render_trs[depth++] = arrays[0] + (uint64_t)index * sizeof(MESH_TRS);
        if (!rr_read(arrays[1] + (uint64_t)index * sizeof(index), &index, sizeof(index))) return;
    }
    if (index == -1) {
        vehicle->render_depth = depth;
        vehicle->render_transform = transform;
        vehicle->render_native = native;
        vehicle->render_hierarchy = hierarchy;
        vehicle->render_index = rr_i32(native + 0x30);
        vehicle->render_view_at = GetTickCount();
    }
}

/* Match model IDs to the vehicle roots used by the rendered scene. */
static void cache_vehicle_views(VEH_CACHE* vehicles, int count) {
    uint64_t updater=g_views_updater, battle=g_battle;
    if (!views_updater_valid(updater,battle) || rr_u64(battle+RR_OFF_BATTLE_WORLD)!=g_world) return;
    for (int kind=0;kind<2;++kind) {
        uint64_t dict=rr_u64(updater+(kind?RR_OFF_VIEWS_HELICOPTERS:RR_OFF_VIEWS_CARS)), entries=0;
        int n=0,stride=0;
        if (!u_dict(dict,&n,&entries,&stride) || n>4096 || stride!=0x18) continue;
        for (int i=0;i<n;++i) {
            uint8_t entry[0x18]; int32_t id; uint64_t view;
            if (!rr_read(entries+(uint64_t)i*0x18,entry,sizeof(entry))) break;
            memcpy(&id,entry+8,4); memcpy(&view,entry+0x10,8);
            if (!PL(view)) continue;
            int j; for (j=0;j<count;++j) if (vehicles[j].kind==kind && vehicles[j].id==id) break;
            if (j==count || !class_named(rr_u64(view),kind?"HelicopterEntityView":"CarEntityView")) continue;
            if (br_get()->vehicle_chams)
                cache_vehicle_render_root(&vehicles[j], rr_u64(view + (kind ? 0x30 : 0x28)));
#ifdef RR_PORT_DIAGNOSTICS
            if (!br_get()->show_vehicles) continue;
            uint64_t ve=rr_u64(view+0x20);
            if (!PL(ve) || !class_named(rr_u64(ve),"VehicleEntityView")) continue;
            uint64_t marker=rr_u64(ve+0x58);
            if (!PL(marker) || !class_named(rr_u64(marker),"VehicleMarkerRootView")) continue;
            uint64_t tr=rr_u64(marker+0x20);
            if (!PL(tr) || !class_named(rr_u64(tr),"Transform")) continue;
            uint64_t native=rr_u64(tr+0x10), h=PL(native)?rr_u64(native+0x28):0;
            int idx=PL(native)?rr_i32(native+0x30):-1;
            int capacity=PL(h)?rr_i32(h+0x10):0;
            uint64_t pointers=PL(h)?rr_u64(h+0x30):0;
            if (capacity<1 || capacity>2048 || idx<0 || idx>=capacity || !PL(pointers) ||
                rr_u64(pointers+(uint64_t)idx*8)!=native) continue;
            vehicles[j].marker_transform=tr; vehicles[j].marker_native=native;
            vehicles[j].marker_h=h; vehicles[j].marker_idx=idx;
#endif
        }
    }
}

/* Authoritative world dictionaries contain only currently replicated cars and
 * helicopters. Validate both dictionary key and object ID before publishing. */
static void vehicle_owner_role(uint64_t object, int kind, char* role, size_t capacity) {
    if (!capacity) return;
    role[0] = 0;
    uint64_t model = rr_u64(object + (kind ? RR_OFF_HELI_OWNER_ROLE : RR_OFF_CAR_OWNER_ROLE));
    if (!u_is(model, "PrimitiveModel`1")) return;
    uint64_t data = rr_u64(model + 0x10);
    if (!u_is(data, "ReferenceData`1")) return;
    uint64_t description = rr_u64(data + RR_OFF_REF_VALUE);
    if (!u_is(description, "PlayerRoleDescription")) return;
    if (!u_string(rr_u64(description + RR_OFF_DESC_NAME), role, capacity)) role[0] = 0;
}

static void cache_vehicles(void) {
    uint64_t world = g_world, klass = 0;
    if (!PL(world) || !rr_read(world, &klass, 8) || klass != g_k_world) return;
    int count = 0;
    for (int kind = 0; kind < 2; ++kind) {
        int begin = count, failed = 0;
        uint64_t dict = rr_u64(world + (kind ? RR_OFF_WEM_HELICOPTERS : RR_OFF_WEM_CARS));
        int used = 0;
        if (!rr_ok() || (dict && (!PL(dict) || !rr_read(dict + U_DICT_COUNT, &used, 4) || used < 0 || used > 4096))) failed = 1;
        int n = 0, stride = 0; uint64_t entries = 0;
        if (!failed && used && (!u_dict(dict, &n, &entries, &stride) || stride != 0x18 || n != used)) failed = 1;
        for (int i = 0; !failed && i < n && count < RR_MAX_VEHICLES; ++i) {
            uint8_t entry[0x18], head[(RR_OFF_CAR_ID > RR_OFF_HELI_ID ? RR_OFF_CAR_ID : RR_OFF_HELI_ID) + 4];
            int32_t key, id; uint64_t obj, k;
            if (!rr_read(entries + (uint64_t)i * 0x18, entry, sizeof(entry))) { failed = 1; break; }
            memcpy(&key, entry + 8, 4); memcpy(&obj, entry + 0x10, 8);
            if (!PL(obj) || !rr_read(obj, head, sizeof(head))) continue;
            memcpy(&k, head, 8); memcpy(&id, head + (kind ? RR_OFF_HELI_ID : RR_OFF_CAR_ID), 4);
            if (id != key || !class_named(k, kind ? "HelicopterModel" : "CarModel")) continue;
            uint64_t tr = rr_u64(obj + (kind ? RR_OFF_HELI_TRANSFORM : RR_OFF_CAR_TRANSFORM));
            if (!PL(tr)) continue;
            VEH_CACHE* v = &g_vehicles_stage[count];
            memset(v, 0, sizeof(*v));
            v->obj = obj; v->klass = k; v->tr = tr; v->id = id; v->kind = kind;
            vehicle_owner_role(obj, kind, v->role, sizeof(v->role));
            int want_seats = br_get()->show_vehicle_markers && br_get()->show_vehicle_occupants;
#ifdef RR_PORT_DIAGNOSTICS
            want_seats |= br_get()->show_vehicles;
#endif
            if (want_seats) {
                uint64_t seats=rr_u64(obj+(kind?RR_OFF_HELI_SEATS:RR_OFF_CAR_SEATS));
                v->occupants_known=vehicle_seat_count(seats,&v->occupants);
            }
            uint64_t desc = rr_u64(obj + (kind ? RR_OFF_HELI_DESC : RR_OFF_CAR_DESC));
            uint64_t name = PL(desc) ? rr_u64(desc + 0x10) : 0;
            if (PL(name)) u_string(name, v->name, sizeof(v->name));
            if (!v->name[0]) snprintf(v->name, sizeof(v->name), "%s", kind ? "HELICOPTER" : "CAR");
            ++count;
        }
        if (failed) {
            count = begin;
            if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
            for (int i = 0; g_vehicle_world == world && i < g_nvehicles && count < RR_MAX_VEHICLES; ++i)
                if (g_vehicles[i].kind == kind) {
                    g_vehicles_stage[count] = g_vehicles[i];
                    g_vehicles_stage[count].occupants_known=0;
                    g_vehicles_stage[count++].occupants=0;
                }
            if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
        }
    }
    if (world != g_world) return;
    for (int i = 0; i < count; ++i) g_vehicles_stage[i].render_depth = 0;
    cache_vehicle_views(g_vehicles_stage,count);
    if (world != g_world) return;
    static VEH_CACHE previous_vehicles[RR_MAX_VEHICLES];
    int previous_count=0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    if (g_vehicle_world == world) {
        previous_count=g_nvehicles;
        if (previous_count>0) memcpy(previous_vehicles,g_vehicles,
                                     (size_t)previous_count*sizeof(previous_vehicles[0]));
    }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    /* Keep a recently validated render root through an incomplete view-table
     * refresh. The model, transform, hierarchy and native slot must still
     * match; sampling validates the TRS chain again before it is drawn. */
    for (int i=0; i<count; ++i) {
        VEH_CACHE* v=&g_vehicles_stage[i];
        if (v->render_depth) continue;
        for (int j=0; j<previous_count; ++j) {
            const VEH_CACHE* old=&previous_vehicles[j];
            if (old->obj!=v->obj || old->id!=v->id || old->kind!=v->kind ||
                !old->render_depth || !old->render_view_at ||
                GetTickCount()-old->render_view_at>1500) continue;
            uint64_t h=old->render_hierarchy, native=old->render_native;
            int capacity=PL(h)?rr_i32(h+0x10):0;
            uint64_t slots=PL(h)?rr_u64(h+0x30):0;
            if (old->render_index<0 || old->render_index>=capacity || capacity>65536 ||
                !PL(slots) || rr_u64(old->render_transform+0x10)!=native ||
                rr_u64(native+0x28)!=h || rr_i32(native+0x30)!=old->render_index ||
                rr_u64(slots+(uint64_t)old->render_index*8)!=native) break;
            memcpy(v->render_trs,old->render_trs,sizeof(v->render_trs));
            v->render_depth=old->render_depth;
            v->render_transform=old->render_transform; v->render_native=native;
            v->render_hierarchy=h; v->render_index=old->render_index;
            v->render_view_at=old->render_view_at;
            break;
        }
    }
    if (world != g_world) return;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_vehicles, g_vehicles_stage, sizeof(VEH_CACHE) * (size_t)count);
    g_nvehicles = count;
    g_vehicle_world = world;
    ++g_vehicle_generation;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
}

#ifdef RR_INTERNAL
/* Unity 2026-11-13 scene APIs, called only from the Cinemachine callback. */
#define UNITY_FN(rva, type) ((type)(uintptr_t)(g_module + (rva)))
typedef uint64_t (*SC_GET_OBJ)(void*);
typedef uint64_t (*SC_GET_REF)(uint64_t, void*);
typedef void (*SC_SET_REF)(uint64_t, uint64_t, void*);
typedef RR_V3* (*SC_GET_POS)(RR_V3*, uint64_t, void*);
typedef RR_Q4* (*SC_GET_COLOR)(RR_Q4*, uint64_t, void*);
typedef void (*SC_SET_COLOR)(uint64_t, const RR_Q4*, void*);
typedef int (*SC_GET_INT)(uint64_t, void*);
typedef void (*SC_SET_INT)(uint64_t, int, void*);
typedef float (*SC_GET_FLOAT)(uint64_t, void*);
typedef void (*SC_SET_FLOAT)(uint64_t, float, void*);

static int scene_alive(uint64_t obj) {
    return PL(obj) && PL(rr_u64(obj + 0x10));
}

static RR_Q4 scene_color(const BR_Config* cfg, int slot) {
    return (RR_Q4){cfg->color[slot].rgb[0]/255.0f, cfg->color[slot].rgb[1]/255.0f,
                   cfg->color[slot].rgb[2]/255.0f, 1.0f};
}

static uint64_t scene_type(const char* name) {
    typedef uint64_t (*TYPE_FN)(uint64_t);
    HMODULE ga = (HMODULE)(uintptr_t)g_module;
    TYPE_FN class_type = (TYPE_FN)GetProcAddress(ga, "il2cpp_class_get_type");
    TYPE_FN type_object = (TYPE_FN)GetProcAddress(ga, "il2cpp_type_get_object");
    const uint64_t klass = resolve_class_api(name);
    return class_type && type_object && klass ? type_object(class_type(klass)) : 0;
}

static void scene_atmosphere_frame(const BR_Config* cfg) {
    static const unsigned fields[] = {0x84, 0x98, 0x240, 0x258, 0x268, 0x278, 0x288,
                                      0x2CC, 0x2D8, 0x2E0, 0x324};
    static const unsigned sizes[] = {1, 4, 1, 16, 16, 16, 16, 4, 4, 1, 1};
    static unsigned char original[_countof(fields)][16];
    static uint64_t settings, controller, sky_type, world;
    static DWORD attempted;
    const int custom_fog = cfg->world_fog_custom && !cfg->world_fog_off;
    const int active = cfg->world_fog_off || custom_fog || cfg->world_sky_glow;
    if (settings && (!active || world != g_world || !scene_alive(controller) ||
                     rr_u64(controller + 0x20) != settings)) {
        if (scene_alive(settings) && u_is(settings, "SkySettings"))
            for (int i=0; i<_countof(fields); ++i)
                memcpy((void*)(uintptr_t)(settings+fields[i]), original[i], sizes[i]);
        settings = 0;
    }
    if (!active) return;
    if (!settings) {
        const DWORD now = GetTickCount();
        if (world == g_world && now-attempted < 1000) return;
        attempted = now;
        world = g_world;
        if (!sky_type) sky_type = scene_type("SkyController");
        if (!sky_type) return;
        controller = UNITY_FN(0x628E690u, SC_GET_REF)(sky_type, NULL);
        const uint64_t candidate = scene_alive(controller) && u_is(controller, "SkyController")
            ? rr_u64(controller + 0x20) : 0;
        if (!scene_alive(candidate) || !u_is(candidate, "SkySettings")) return;
        for (int i=0; i<_countof(fields); ++i)
            if (!rr_read(candidate+fields[i], original[i], sizes[i])) return;
        settings = candidate;
    }

    const unsigned char on = 1, off = 0;
    const float glow = cfg->world_sky_glow * .01f;
    const float density = cfg->world_fog_density * .001f;
    const float distance = (float)cfg->world_fog_distance;
    const RR_Q4 color = scene_color(cfg, BR_COLOR_FOG);
    const void* values[] = {
        cfg->world_sky_glow ? &on : original[0],
        cfg->world_sky_glow ? (const void*)&glow : original[1],
        cfg->world_fog_off ? &off : custom_fog ? &on : original[2],
        custom_fog ? (const void*)&color : original[3],
        custom_fog ? (const void*)&color : original[4],
        custom_fog ? (const void*)&color : original[5],
        custom_fog ? (const void*)&color : original[6],
        custom_fog ? (const void*)&density : original[7],
        custom_fog ? (const void*)&distance : original[8],
        custom_fog ? &off : original[9], custom_fog ? &off : original[10]
    };
    for (int i=0; i<_countof(fields); ++i)
        memcpy((void*)(uintptr_t)(settings+fields[i]), values[i], sizes[i]);
}

static void world_lighting_frame(uint64_t camera, const BR_Config* cfg) {
    static uint64_t light, settings_world, sky_camera;
    static int sky_flags, fog_original;
    static float sunlight;
    static RR_Q4 sky_color;
    const uint64_t world = g_world;
    const int sky_on = cfg->world_sky_color || cfg->world_sky_off;

    if (sky_camera && (!sky_on || sky_camera != camera || settings_world != world)) {
        if (scene_alive(sky_camera)) {
            UNITY_FN(0x61E2FD0u, SC_SET_COLOR)(sky_camera, &sky_color, NULL);
            UNITY_FN(0x61E3320u, SC_SET_INT)(sky_camera, sky_flags, NULL);
        }
        sky_camera = 0;
    }
    if (settings_world != world) {
        settings_world = world;
        fog_original = 0;
    }
    if (cfg->world_sun || light) {
        const uint64_t sun = UNITY_FN(0x6211280u, SC_GET_OBJ)(NULL);
        if (light && (sun != light || !cfg->world_sun)) {
            if (scene_alive(light)) UNITY_FN(0x62271D0u, SC_SET_FLOAT)(light, sunlight, NULL);
            light = 0;
        }
        if (cfg->world_sun && scene_alive(sun)) {
            if (!light) {
                sunlight = UNITY_FN(0x62253C0u, SC_GET_FLOAT)(sun, NULL);
                if (isfinite(sunlight) && sunlight >= 0 && sunlight < 100) light = sun;
            }
            if (light) UNITY_FN(0x62271D0u, SC_SET_FLOAT)(light, sunlight*cfg->world_sun_pct/100.0f, NULL);
        }
    }
    scene_atmosphere_frame(cfg);
    if (cfg->world_fog_off || fog_original) {
        typedef unsigned char (*GET_FOG)(void*);
        typedef void (*SET_FOG)(unsigned char, void*);
        if (!fog_original) fog_original = 1 + UNITY_FN(0x6210F60u, GET_FOG)(NULL);
        UNITY_FN(0x6211910u, SET_FOG)(cfg->world_fog_off ? 0 : fog_original - 1, NULL);
        if (!cfg->world_fog_off) fog_original = 0;
    }
    if (sky_on && scene_alive(camera)) {
        if (!sky_camera) {
            sky_flags = UNITY_FN(0x61DFD00u, SC_GET_INT)(camera, NULL);
            UNITY_FN(0x61DF900u, SC_GET_COLOR)(&sky_color, camera, NULL);
            sky_camera = camera;
        }
        const RR_Q4 clear = cfg->world_sky_color ? scene_color(cfg, BR_COLOR_SKY) : sky_color;
        UNITY_FN(0x61E2FD0u, SC_SET_COLOR)(camera, &clear, NULL);
        UNITY_FN(0x61E3320u, SC_SET_INT)(camera, 2, NULL);  /* CameraClearFlags.SolidColor */
    }
}

extern void rr_int_log(const char* fmt, ...);
#endif

/* Refresh only the displayed pose just before painting; keep the snapshot's
 * validated lens and leave the aim worker on its coherent snapshot. */
int game_refresh_render_camera(RR_CAMERA* cam) {
    uint64_t model = g_cam_obj, world = g_world;
    uint8_t buf[0x50];
    if (!cam || !cam->valid || cam->model_source != model || !PL(model) || !PL(world) ||
        !rr_read(model + RR_OFF_CAM_POS, buf, sizeof(buf))) return 0;
    RR_CAMERA next = *cam;
    memcpy(&next.pos, buf, sizeof(RR_V3));
    memcpy(&next.rot, buf + (RR_OFF_CAM_ROT - RR_OFF_CAM_POS), sizeof(RR_Q4));
    memcpy(&next.fwd, buf + (RR_OFF_CAM_FWD - RR_OFF_CAM_POS), sizeof(RR_V3));
    memcpy(&next.right, buf + (RR_OFF_CAM_RIGHT - RR_OFF_CAM_POS), sizeof(RR_V3));
    memcpy(&next.up, buf + (RR_OFF_CAM_UP - RR_OFF_CAM_POS), sizeof(RR_V3));
    float fl = next.fwd.x*next.fwd.x + next.fwd.y*next.fwd.y + next.fwd.z*next.fwd.z;
    float rl = next.right.x*next.right.x + next.right.y*next.right.y + next.right.z*next.right.z;
    float ul = next.up.x*next.up.x + next.up.y*next.up.y + next.up.z*next.up.z;
    float fr = next.fwd.x*next.right.x + next.fwd.y*next.right.y + next.fwd.z*next.right.z;
    float fu = next.fwd.x*next.up.x + next.fwd.y*next.up.y + next.fwd.z*next.up.z;
    float ru = next.right.x*next.up.x + next.right.y*next.up.y + next.right.z*next.up.z;
    float dx = next.pos.x-cam->pos.x, dy = next.pos.y-cam->pos.y, dz = next.pos.z-cam->pos.z;
    if (!isfinite(next.pos.x) || !isfinite(next.pos.y) || !isfinite(next.pos.z) ||
        !isfinite(fl) || !isfinite(rl) || !isfinite(ul) ||
        fl < .81f || fl > 1.21f || rl < .81f || rl > 1.21f || ul < .81f || ul > 1.21f ||
        fabsf(fr) > .05f || fabsf(fu) > .05f || fabsf(ru) > .05f ||
        dx*dx + dy*dy + dz*dz > 100.0f ||
        model != g_cam_obj || world != g_world) return 0;
    *cam = next;
    return 1;
}

/* Init. */

/* VAD discovery. */
/* Rebuild the region list from the target process before discovery.
 * Committed regions can be freed or reused during a session, so saved ranges
 * are search hints rather than proof that an address remains readable.
 */


static int list_regions(RR_HEAP* out, int max, uint64_t min_bytes) {
    uint64_t a = 0x10000ULL, hi = 0x7FFFFFFFFFFFULL;
    int n = 0;
    while (a < hi && n < max) {
        RR_REGION r;
        if (!rr_query_region(a, &r)) { a += 0x10000; continue; }
        if (r.size == 0) break;
        if (r.state == 0x1000 && r.size >= min_bytes && (r.protect & 0xEE)) {
            out[n].base = r.base;
            out[n].size = r.size;
            n++;
        }
        uint64_t next = r.base + r.size;
        if (next <= a) break;
        a = next;
    }
    return n;
}

static void find_heap_regions(void) {
    g_nregions = list_regions(g_regions, RR_MAX_REGIONS, 1ULL << 12);
    g_nheap = 0;
    for (int k = 0; k < g_nregions && g_nheap < RR_MAX_HEAP; k++)
        g_heap[g_nheap++] = g_regions[k];
}

/* Parallel pattern scan inside one region (metadata strings). */
typedef struct {
    uint64_t lo, hi;
    const uint8_t* pat;
    int plen;
    volatile LONG found;
    uint64_t addr;
} PAT_CTX;

static DWORD WINAPI pat_thread(LPVOID p) {
    PAT_CTX* c = (PAT_CTX*)p;
    uint8_t buf[0x10000 + 128];
    unsigned chunks = 0;
    for (uint64_t a = c->lo; a < c->hi; a += 0x10000) {
        if (InterlockedCompareExchange(&c->found, 0, 0)) return 0;
        if ((++chunks & 63) == 0) Sleep(1);
        size_t n = (size_t)((c->hi - a) < 0x10000 ? (c->hi - a) : 0x10000);
        if (!rr_read(a, buf, n)) continue;
        for (size_t i = 0; i + (size_t)c->plen <= n; i++) {
            if (memcmp(buf + i, c->pat, (size_t)c->plen) != 0) continue;
            c->addr = a + i;
            InterlockedExchange(&c->found, 1);
            return 0;
        }
    }
    return 0;
}

static uint64_t scan_pattern(uint64_t base, uint64_t size, const uint8_t* pat, int plen) {
    if (size < (uint64_t)plen) return 0;
    PAT_CTX c[SCAN_THREADS];
    HANDLE th[SCAN_THREADS];
    uint64_t span = size / SCAN_THREADS;
    if (span < 0x10000) span = 0x10000;
    for (int i = 0; i < SCAN_THREADS; i++) {
        c[i].lo = base + span * (uint64_t)i;
        c[i].hi = (i == SCAN_THREADS - 1) ? base + size : base + span * (uint64_t)(i + 1);
        c[i].pat = pat;
        c[i].plen = plen;
        c[i].found = 0;
        c[i].addr = 0;
        th[i] = rr_thread_start(pat_thread, &c[i]);
    }
    /* A job that never started runs here; these contexts live on this stack
     * frame and a NULL handle in the wait set makes the wait fail instantly. */
    for (int i = 0; i < SCAN_THREADS; i++)
        if (!th[i]) pat_thread(&c[i]);
    for (int i = 0; i < SCAN_THREADS; i++)
        if (th[i] && WaitForSingleObject(th[i], INFINITE) != WAIT_OBJECT_0) pat_thread(&c[i]);
    uint64_t hit = 0;
    for (int i = 0; i < SCAN_THREADS; i++) {
        if (th[i]) CloseHandle(th[i]);
        if (c[i].found) hit = c[i].addr;
    }
    return hit;
}

/* Resolve an Il2CppClass without any cached address: scan the committed
 * regions for the class name string, then look for the class struct that
 * points at it. */
/* Class resolution. */
/* Resolve a class in three stages: collect name strings, collect candidate
 * class structures that reference them, then seek a plausible live instance.
 * These checks reduce false matches in metadata that repeats the same name.
 */

#define MAX_HITS 64
#define MAX_CANDS 16

static uint64_t g_hits[MAX_HITS];
static volatile LONG g_nhits;
static uint64_t g_cands[MAX_CANDS];
static int g_ncands;

typedef struct {
    const RR_HEAP* regions;
    int count;
    int start, stride;
    const uint8_t* pat;
    int plen;
} HIT_JOB;

/* One job per worker over a strided slice of the region list. Spawning and
 * waiting on SCAN_THREADS jobs for every region turned a class lookup into
 * tens of thousands of thread-pool round trips (70 s on a loaded session). */
static DWORD WINAPI hit_job_thread(LPVOID p) {
    HIT_JOB* j = (HIT_JOB*)p;
    uint8_t buf[0x10000 + 128];
    unsigned chunks = 0;
    for (int r = j->start; r < j->count; r += j->stride) {
        if (InterlockedCompareExchange(&g_nhits, 0, 0) >= MAX_HITS) return 0;
        const RR_HEAP* reg = &j->regions[r];
        if (reg->size < (1ULL << 20)) continue;
        for (uint64_t a = reg->base; a < reg->base + reg->size; a += 0x10000) {
            size_t n = (size_t)((reg->base + reg->size - a) < 0x10000
                                    ? (reg->base + reg->size - a) : 0x10000);
            if ((++chunks & 63) == 0) Sleep(1);
            if (!rr_read(a, buf, n)) continue;
            for (size_t i = 0; i + (size_t)j->plen <= n; i++) {
                if (memcmp(buf + i, j->pat, (size_t)j->plen) != 0) continue;
                LONG k = InterlockedIncrement(&g_nhits) - 1;
                if (k < MAX_HITS) g_hits[k] = a + i;
            }
            if (InterlockedCompareExchange(&g_nhits, 0, 0) >= MAX_HITS) return 0;
        }
    }
    return 0;
}

static void collect_hits(const char* name) {
    uint8_t pat[96];
    size_t len = strlen(name);
    if (len >= sizeof(pat)) return;
    memcpy(pat, name, len);
    g_nhits = 0;
    HIT_JOB jobs[SCAN_THREADS];
    HANDLE th[SCAN_THREADS];
    for (int i = 0; i < SCAN_THREADS; i++) {
        jobs[i].regions = g_regions;
        jobs[i].count = g_nregions;
        jobs[i].start = i;
        jobs[i].stride = SCAN_THREADS;
        jobs[i].pat = pat;
        jobs[i].plen = (int)len;
        th[i] = rr_thread_start(hit_job_thread, &jobs[i]);
    }
    WaitForMultipleObjects(SCAN_THREADS, th, TRUE, INFINITE);
    for (int i = 0; i < SCAN_THREADS; i++) CloseHandle(th[i]);
}

static int is_hit(uint64_t v) {
    LONG n = g_nhits < MAX_HITS ? g_nhits : MAX_HITS;
    for (LONG i = 0; i < n; i++) if (g_hits[i] == v) return 1;
    return 0;
}

static void add_cand(uint64_t k) {
    for (int i = 0; i < g_ncands; i++) if (g_cands[i] == k) return;
    if (g_ncands < MAX_CANDS) g_cands[g_ncands++] = k;
}

/* One pass over the committed regions: any qword matching a name occurrence is
 * a potential Il2CppClass.name pointer. */
static void collect_candidates(const char* name) {
    g_ncands = 0;
    static uint8_t buf[0x10000];
    for (int r = 0; r < g_nregions && g_ncands < MAX_CANDS; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (!is_hit(v)) continue;
                uint64_t k = a + i - 0x10;
                if (class_named(k, name)) add_cand(k);
            }
        }
    }
}

/* Accept candidates with a matching class word, a readable transform
 * pointer and finite coordinates. These are plausibility checks.
 */
static int probe_class(uint64_t klass) {
    static uint8_t buf[0x10000];
    for (int r = 0; r < g_nregions; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != klass) continue;
                uint64_t obj = a + i;
                uint64_t tr = rr_u64(obj + RR_OFF_CHAR_TRANSFORM);
                if (!PL(tr)) continue;
                float f[6];
                if (!rr_read(tr + RR_OFF_CTM_X, f, sizeof(f))) continue;
                if (!isfinite(f[0]) || !isfinite(f[1]) || !isfinite(f[2])) continue;
                if (fabsf(f[0]) > 500000.0f || fabsf(f[1]) > 500000.0f || fabsf(f[2]) > 500000.0f) continue;
                return 1;
            }
        }
    }
    return 0;
}

/* Camera classes cannot be validated with the character probe: their fields are
 * floats, not a transform pointer. Probe with camera_validate instead. */
static int probe_camera_class(uint64_t klass) {
    static uint8_t buf[0x10000];
    for (int r = 0; r < g_nregions; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != klass) continue;
                if (camera_validate(a + i)) return 1;
            }
        }
    }
    return 0;
}

static uint64_t resolve_camera_class(const char* name) {
    uint64_t api = resolve_class_api(name);
    if (api) return api;
    collect_hits(name);
    if (!g_nhits) return 0;
    collect_candidates(name);
    for (int i = 0; i < g_ncands; i++)
        if (probe_camera_class(g_cands[i])) return g_cands[i];
    return 0;
}

/* Class lookup through GameAssembly's own exported API. The name scan below
 * walks every committed region for the class-name string and then every region
 * again for a pointer to it; on a loaded client that is tens of seconds per
 * call, which is why a fresh injection sat on "BattleCharacterModel class not
 * found" for minutes. The domain/assembly/image calls return the same
 * Il2CppClass* in microseconds. */
typedef void*        (*RR_DOMAIN_GET)(void);
typedef const void* const* (*RR_DOMAIN_GET_ASM)(void*, size_t*);
typedef const void*  (*RR_ASM_IMAGE)(const void*);
typedef size_t       (*RR_IMAGE_CLASS_COUNT)(const void*);
typedef const void*  (*RR_IMAGE_CLASS)(const void*, size_t);
typedef const void*  (*RR_CLASS_FROM_NAME)(const void*, const char*, const char*);

static uint64_t resolve_class_api(const char* name) {
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (!ga) return 0;
    RR_DOMAIN_GET domain_get = (RR_DOMAIN_GET)(void*)GetProcAddress(ga, "il2cpp_domain_get");
    RR_DOMAIN_GET_ASM get_asms = (RR_DOMAIN_GET_ASM)(void*)GetProcAddress(ga, "il2cpp_domain_get_assemblies");
    RR_ASM_IMAGE asm_image = (RR_ASM_IMAGE)(void*)GetProcAddress(ga, "il2cpp_assembly_get_image");
    RR_IMAGE_CLASS_COUNT img_count = (RR_IMAGE_CLASS_COUNT)(void*)GetProcAddress(ga, "il2cpp_image_get_class_count");
    RR_IMAGE_CLASS img_class = (RR_IMAGE_CLASS)(void*)GetProcAddress(ga, "il2cpp_image_get_class");
    RR_CLASS_FROM_NAME from_name = (RR_CLASS_FROM_NAME)(void*)GetProcAddress(ga, "il2cpp_class_from_name");
    if (!domain_get || !get_asms || !asm_image || (!img_class && !from_name)) return 0;
    void* domain = domain_get();
    if (!domain) return 0;
    size_t count = 0;
    const void* const* asms = get_asms(domain, &count);
    if (!asms || count < 1 || count > 4096) return 0;
    /* Image traversal allocates on first touch; the GC has to know this thread. */
    cursor_attach_thread(ga);
    uint64_t found = 0;
    __try {
        for (size_t a = 0; a < count && !found; ++a) {
            if (!asms[a]) continue;
            const void* image = asm_image(asms[a]);
            if (!image) continue;
            if (img_count && img_class) {
                const size_t classes = img_count(image);
                if (classes > 200000) continue;
                for (size_t c = 0; c < classes; ++c) {
                    const uint64_t k = (uint64_t)(uintptr_t)img_class(image, c);
                    if (class_named(k, name)) { found = k; break; }
                }
            }
            /* Older runtimes export class_from_name but not the image table. */
            if (!found && from_name) {
                static const char* const spaces[] = {
                    "", "Models.Models.BattleCharacter", "Models.Models.Battle",
                    "Models.Models", "Game", "Roblox"
                };
                for (size_t n = 0; n < sizeof spaces / sizeof spaces[0] && !found; ++n) {
                    const uint64_t k = (uint64_t)(uintptr_t)from_name(image, spaces[n], name);
                    if (class_named(k, name)) found = k;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { found = 0; }
    return found;
}

static uint64_t resolve_class(const char* name) {
    uint64_t api = resolve_class_api(name);
    if (api) {
        snprintf(g_dbg, sizeof(g_dbg), "api=%llX", (unsigned long long)api);
        return api;
    }
    collect_hits(name);
    snprintf(g_dbg, sizeof(g_dbg), "hits=%ld", (long)(g_nhits < MAX_HITS ? g_nhits : MAX_HITS));
    if (!g_nhits) return 0;
    collect_candidates(name);
    snprintf(g_dbg, sizeof(g_dbg), "hits=%ld cands=%d",
             (long)(g_nhits < MAX_HITS ? g_nhits : MAX_HITS), g_ncands);
    for (int i = 0; i < g_ncands; i++) {
        if (probe_class(g_cands[i])) {
            snprintf(g_dbg, sizeof(g_dbg), "hits=%ld cands=%d accepted=%llX",
                     (long)(g_nhits < MAX_HITS ? g_nhits : MAX_HITS), g_ncands,
                     (unsigned long long)g_cands[i]);
            return g_cands[i];
        }
    }
    return 0;
}

/* Transform pool. */
typedef struct {
    const ENT_CACHE* src;
    const uint16_t*  idx;
    int              count;
    int              need;
    float           (*dst)[6];
    uint8_t*         ok;
    volatile LONG    next;
    volatile LONG    done;
    volatile LONG    run;
} XF_JOB;

static XF_JOB   g_xf;
static HANDLE   g_xf_sem;
static HANDLE   g_xf_done;
static int      g_xf_started;

static DWORD WINAPI xf_thread(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_xf_sem, INFINITE);
        if (!InterlockedCompareExchange(&g_xf.run, 0, 0)) return 0;
        for (;;) {
            LONG k = InterlockedIncrement(&g_xf.next) - 1;
            if (k >= g_xf.count) break;
            int i = g_xf.idx ? g_xf.idx[k] : (int)k;
            if (i >= RR_MAX_PLAYERS) continue;
            g_xf.ok[i] = rr_read(g_xf.src[i].tr + RR_OFF_CTM_X, g_xf.dst[i], TR_READ_LEN) ? 1 : 0;
        }
        if (InterlockedIncrement(&g_xf.done) == g_xf.need) SetEvent(g_xf_done);
    }
    return 0;
}

static void xf_pool_init(void) {
    if (g_xf_started) return;
    g_xf_sem = CreateSemaphore(NULL, 0, XF_THREADS * 4, NULL);
    g_xf_done = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_xf_sem || !g_xf_done) return;
    for (int i = 0; i < XF_THREADS; i++)
        rr_thread_start(xf_thread, NULL);
    g_xf_started = 1;
}

static int xf_read_idx(const ENT_CACHE* src, const uint16_t* idx, int count,
                       float (*dst)[6], uint8_t* ok) {
    if (count <= 0) return 0;

    /* Prefer the batch IOCTL when supported. Retry it periodically after a
     * failure so a transient transport error does not permanently select the
     * thread-pool fallback.
     */
    static unsigned batch_retries;
    if (g_batch_state < 0 && (++batch_retries % 128u) == 0u) g_batch_state = 0;
    if (g_batch_state >= 0 && count <= BATCH_MAX) {
        static RR_RD_REQ reqs[BATCH_MAX];
        for (int k = 0; k < count; k++) {
            int i = idx[k];
            reqs[k].addr = src[i].tr + RR_OFF_CTM_X;
            reqs[k].size = TR_READ_LEN;
            reqs[k].dst = dst[i];
        }
        int r = rr_read_batch(reqs, count);
        if (r == -1) {
            g_batch_state = -1;
        } else {
            int good = 0;
            for (int k = 0; k < count; k++) {
                int i = idx[k];
                const float* f = dst[i];
                ok[i] = (f[0] != 0.0f || f[1] != 0.0f || f[2] != 0.0f ||
                         f[3] != 0.0f || f[4] != 0.0f || f[5] != 0.0f) ? 1 : 0;
                good += ok[i];
            }
            return good;
        }
    }

    if (!g_xf_started || count <= 3) {
        for (int k = 0; k < count; k++) {
            int i = idx[k];
            ok[i] = rr_read(src[i].tr + RR_OFF_CTM_X, dst[i], TR_READ_LEN) ? 1 : 0;
        }
        return count;
    }

    g_xf.src = src;
    g_xf.idx = idx;
    g_xf.count = count;
    g_xf.dst = dst;
    g_xf.ok = ok;
    g_xf.next = 0;
    g_xf.done = 0;
    int workers = count < XF_THREADS ? count : XF_THREADS;
    g_xf.need = workers;
    InterlockedExchange(&g_xf.run, 1);
    ReleaseSemaphore(g_xf_sem, workers, NULL);
    WaitForSingleObject(g_xf_done, 50);
    return count;
}

static int xf_read(const ENT_CACHE* src, int n, float (*dst)[6], uint8_t* ok) {
    static uint16_t all[RR_MAX_PLAYERS];
    int c = n < RR_MAX_PLAYERS ? n : RR_MAX_PLAYERS;
    for (int i = 0; i < c; i++) all[i] = (uint16_t)i;
    return xf_read_idx(src, all, c, dst, ok);
}

/* Skeleton. */
typedef struct { uint64_t cfg; int count, stride, loff, length, group[4]; int ok; } SKELCFG;
static SKELCFG g_skelcfg;

static int skel_config(uint64_t cfg) {
    if (g_skelcfg.ok && g_skelcfg.cfg == cfg) return 1;
    memset(&g_skelcfg, 0, sizeof(g_skelcfg));
    int count = rr_i32(cfg + 0x10);
    int stride = rr_i32(cfg + 0x14);
    int loff = rr_i32(cfg + 0x18);
    int length = rr_i32(cfg + 0x20);
    int group[4] = { rr_i32(cfg + 0x48), rr_i32(cfg + 0x44), rr_i32(cfg + 0x40), rr_i32(cfg + 0x38) };
    if (count < 2 || count > RR_MAX_BONES || stride < 9 || stride > 16 || loff < 0 || loff + 3 >= stride || length < 0 || length >= stride) return 0;
    for (int i = 0; i < 4; ++i) if (group[i] < 0 || group[i] >= count) return 0;
    g_skelcfg.cfg = cfg;
    g_skelcfg.count = count;
    g_skelcfg.stride = stride;
    g_skelcfg.loff = loff;
    g_skelcfg.length = length;
    memcpy(g_skelcfg.group, group, sizeof(group));
    g_skelcfg.ok = 1;
    return 1;
}

static int build_bones(uint64_t skel, float yaw, float px, float py, float pz,
                       float out[RR_MAX_BONES][3]) {
    if (!PL(skel)) return 0;
    uint64_t cfg = rr_u64(skel + 0x10);
    uint64_t arr = rr_u64(skel + 0x18);
    if (!PL(cfg) || !PL(arr)) return 0;
    if (!skel_config(cfg)) return 0;
    int count = g_skelcfg.count, stride = g_skelcfg.stride, loff = g_skelcfg.loff;

    float bd[RR_MAX_BONES * 16];
    int need = count * stride;
    int32_t array_length = 0;
    if (!rr_read(arr + 0x18, &array_length, sizeof array_length) || array_length < need) return 0;
    if (!rr_read(arr + 0x20, bd, (size_t)need * 4)) return 0;

    float q[4] = { 0, 0, 0, 1 };
    float p[3] = { 0, 0, 0 };
    float s = sinf(yaw), c = cosf(yaw);
    for (int i = 0; i < count; i++) {
        const float* b = bd + (size_t)i * (size_t)stride;
        float lx = b[loff+0], ly = b[loff+1], lz = b[loff+2], lw = b[loff+3];
        float len = b[g_skelcfg.length];
        float qlen = lx*lx + ly*ly + lz*lz + lw*lw;
        if (!isfinite(qlen) || qlen < 0.25f || qlen > 2.25f || !isfinite(len) || len < 0 || len > 3.0f) return 0;
        float ax=q[0], ay=q[1], az=q[2], aw=q[3];
        q[0] = aw*lx + ax*lw + ay*lz - az*ly;
        q[1] = aw*ly - ax*lz + ay*lw + az*lx;
        q[2] = aw*lz + ax*ly - ay*lx + az*lw;
        q[3] = aw*lw - ax*lx - ay*ly - az*lz;

        float vx = 0, vy = 0, vz = len;
        float cx = q[1]*vz - q[2]*vy;
        float cy = q[2]*vx - q[0]*vz;
        float cz = q[0]*vy - q[1]*vx;
        float dx = vx + 2*(q[3]*cx + q[1]*cz - q[2]*cy);
        float dy = vy + 2*(q[3]*cy + q[2]*cx - q[0]*cz);
        float dz = vz + 2*(q[3]*cz + q[0]*cy - q[1]*cx);
        if (i > 0) { p[0] += dx; p[1] += dy; p[2] += dz; }

        float wx = p[0], wy = p[2], wz = p[1];
        out[i][0] = px + wx * c + wz * s;
        out[i][1] = py + wy;
        out[i][2] = pz - wx * s + wz * c;
        if (!isfinite(out[i][0]) || !isfinite(out[i][1]) || !isfinite(out[i][2]) ||
            fabsf(out[i][0] - px) > 4 || fabsf(out[i][1] - py) > 4 || fabsf(out[i][2] - pz) > 4) return 0;
    }
    return count;
}

/* Unity 6 native TransformAccess: a 48-byte local TRS array and a parent
 * index array. Both are read in two bounded snapshots per character. */
static int mesh_point(const MESH_TRS* trs, const int* parents, int count, int index, RR_V3* out) {
    if (index < 0 || index >= count) return 0;
    RR_V3 v = { trs[index].p[0], trs[index].p[1], trs[index].p[2] };
    int parent = parents[index], steps = 0;
    while (parent >= 0 && parent < count && steps++ < 128) {
        const MESH_TRS* m = &trs[parent];
        float x=v.x*m->s[0], y=v.y*m->s[1], z=v.z*m->s[2];
        float qx=m->q[0], qy=m->q[1], qz=m->q[2], qw=m->q[3];
        float tx=2*(qy*z-qz*y), ty=2*(qz*x-qx*z), tz=2*(qx*y-qy*x);
        v.x=m->p[0]+x+qw*tx+(qy*tz-qz*ty);
        v.y=m->p[1]+y+qw*ty+(qz*tx-qx*tz);
        v.z=m->p[2]+z+qw*tz+(qx*ty-qy*tx);
        parent = parents[parent];
    }
    if (parent != -1 || !isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) return 0;
    *out = v; return 1;
}

#ifdef RR_INTERNAL
static int render_anchor_point(uint64_t native, uint64_t hierarchy, RR_V3* point) {
    if (!PL(native) || !PL(hierarchy) || rr_u64(native + 0x28) != hierarchy) return 0;
    const int slots = rr_i32(hierarchy + 0x10);
    int index = rr_i32(native + 0x30);
    const uint64_t transforms = rr_u64(hierarchy + 0x18);
    const uint64_t parents = rr_u64(hierarchy + 0x20);
    const uint64_t pointers = rr_u64(hierarchy + 0x30);
    if (slots < 1 || slots > 2048 || index < 0 || index >= slots ||
        !PL(transforms) || !PL(parents) || !PL(pointers) ||
        rr_u64(pointers + (uint64_t)index*8) != native) return 0;
    MESH_TRS chain[48];
    int links[_countof(chain)], count = 0;
    while (index >= 0 && index < slots && count < _countof(chain)) {
        if (!rr_read(transforms + (uint64_t)index*sizeof(chain[0]), &chain[count], sizeof(chain[0])))
            return 0;
        index = rr_i32(parents + (uint64_t)index*4);
        links[count] = count+1;
        ++count;
    }
    if (index != -1 || !count) return 0;
    links[count-1] = -1;
    return mesh_point(chain, links, count, 0, point);
}

void game_refresh_render_players(RR_PLAYER* players, int count, const RR_CAMERA* cam) {
    if (!cam->valid) return;
    const BR_Config* cfg = br_get();
    if (!cfg->enabled && !cfg->aa_on) return;
    for (int i=0; i<count; ++i) {
        RR_PLAYER* p = &players[i];
        if (!p->valid || !p->full_rig || p->nbones != RR_CHAM_BONES || p->hidden_in_object ||
            (p->local && !cfg->show_self && !br_local_esp_enabled(cfg)) ||
            (!p->local && cfg->max_distance && p->dist > cfg->max_distance/100.0f)) continue;
        RR_V3 anchor;
        if (!render_anchor_point(p->render_anchor, p->render_hierarchy, &anchor)) continue;
        const float* pelvis = p->bone[RR_RIG_M_SPINE_1_JNT];
        const float dx = anchor.x-pelvis[0], dy = anchor.y-pelvis[1], dz = anchor.z-pelvis[2];
        const float delta = dx*dx+dy*dy+dz*dz;
        if (!(delta < 1.0f && delta > .0001f)) continue;
        p->pos.x+=dx; p->pos.y+=dy; p->pos.z+=dz;
        p->head.x+=dx; p->head.y+=dy; p->head.z+=dz;
        for (int j=0; j<p->nbones; ++j) {
            p->bone[j][0]+=dx; p->bone[j][1]+=dy; p->bone[j][2]+=dz;
            if (p->cham_valid) {
                p->cham_skin[j][3]+=dx; p->cham_skin[j][7]+=dy; p->cham_skin[j][11]+=dz;
            }
        }
    }
}
#endif

static int read_mesh_array(uint64_t address, void* dst, size_t size) {
    for (size_t off=0;off<size;) {
        size_t part=size-off; if (part>0x10000) part=0x10000;
        if (!rr_read(address+off,(uint8_t*)dst+off,part)) return 0;
        off+=part;
    }
    return 1;
}

static void diag_native_names(FILE* f, uint64_t native) {
    for (unsigned off=0;off<0x100;off+=8) {
        uint64_t p=rr_u64(native+off); char name[128];
        if (!PL(p)) continue;
        if (ascii_at(rr_u64(p+0x50),name,sizeof(name)))
            fprintf(f,"  offset=%X possible_gameobject_name=%s\n",off,name);
        if (ascii_at(p,name,sizeof(name)) && (strstr(name,"jnt") || strstr(name,"arm")))
            fprintf(f,"  offset=%X possible_name=%s\n",off,name);
    }
}

static void diag_validate_bone_names(const int* map, const uint64_t* slots) {
    if (!g_pose_diag_enabled) return;
    for (int b=0;b<RR_CHAM_BONES;++b) {
        if(map[b]<0)continue;
        uint64_t native=slots[map[b]], object=PL(native)?rr_u64(native+0x20):0;
        char name[128];
        if (!PL(object) || !ascii_at(rr_u64(object+0x50),name,sizeof(name))) continue;
        ++g_pose_diag.named_joints;
        if (strcmp(name,rr_cham_bones[b].name)) {
            ++g_pose_diag.mismatched_joints; ++g_pose_diag.mismatch_by_bone[b];
            FILE* f=fopen("rig-name-mismatches.log","a");
            if (f) { fprintf(f,"bone=%d expected=%s actual=%s\n",b,rr_cham_bones[b].name,name); fclose(f); }
        }
    }
}

/* Bound the hot-path reads to the mapped joints and their ancestors. */
static void cache_mesh_span(ENT_CACHE* e, const int* parents, int count) {
    int first = count, last = -1;
    for (int i = 0; i < 5 + RR_CHAM_BONES; ++i) {
        int index = i < 5 ? e->mesh_idx[i] : e->cham_idx[i - 5];
        int depth = 0;
        while (index >= 0 && index < count && depth++ < 128) {
            if (index < first) first = index;
            if (index > last) last = index;
            index = parents[index];
        }
        if (index != -1) return;
    }
    e->mesh_first = first;
    e->mesh_span = last - first + 1;
}

/* Bind the complete renderer palette by exact native joint names within the
 * anchored spine subtree. Facial joints and attachment placeholders cannot
 * safely be disambiguated from similar rest translations alone. */
static int native_joint_belongs(uint64_t native, uint64_t hierarchy, int index) {
    uint8_t fields[12]; uint64_t owner; int slot;
    if (!PL(native) || !rr_read(native+0x28,fields,sizeof(fields))) return 0;
    memcpy(&owner,fields,8); memcpy(&slot,fields+8,4);
    return owner==hierarchy && slot==index;
}

/* One-shot record of why a palette failed, written next to the internal log so
 * a session that maps nothing leaves evidence instead of silence. */
static void log_rig_mapping_failure(const ENT_CACHE* e, const int* map,
                                    const uint64_t* slots, int count, int root) {
    static uint64_t recorded;
    uint64_t key = PL(e->mesh_native[0]) ? e->mesh_native[0] : e->mesh_h;
    if (!key || recorded == key) return;
    recorded = key;
    char path[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, (DWORD)sizeof(path));
    if (n > 0 && n + 32 < sizeof(path)) strcat_s(path, sizeof(path), "\\RobRunInternal\\rig-mapping.log");
    FILE* f = path[0] ? fopen(path, "a") : NULL;
    if (!f) f = fopen("rig-mapping.log", "a");
    if (!f) return;
    fprintf(f, "rig %016llX root=%d joints=%d\n", (unsigned long long)key, root, count);
    for (int b = 0; b < RR_CHAM_BONES; ++b)
        if (map[b] < 0)
            fprintf(f, "  missing %2d %s required=%d\n", b, rr_cham_bones[b].name,
                    rr_rig_required_joint(b));
    int shown = 0;
    for (int i = 0; i < count && shown < 32; ++i) {
        uint64_t object = PL(slots[i]) ? rr_u64(slots[i] + 0x20) : 0;
        char name[64];
        if (PL(object) && ascii_at(rr_u64(object + 0x50), name, sizeof(name))) {
            fprintf(f, "  seen[%d] %s\n", i, name);
            ++shown;
        }
    }
    diag_native_names(f, e->mesh_native[0]);
    fclose(f);
}

static void cache_cham_rig(ENT_CACHE* e, const ENT_CACHE* previous) {
    uint64_t h=e->mesh_h, matrices=rr_u64(h+0x18), parent_addr=rr_u64(h+0x20), pointer_addr=rr_u64(h+0x30);
    int count=rr_i32(h+0x10);
    if (count<1 || count>2048 || !PL(matrices) || !PL(parent_addr) || !PL(pointer_addr)) return;
    int parents[2048], map[RR_CHAM_BONES]; uint64_t slots[2048];
    if (!read_mesh_array(pointer_addr,slots,(size_t)count*sizeof(slots[0])) ||
        !read_mesh_array(parent_addr,parents,(size_t)count*sizeof(parents[0]))) return;
    /* The spine root is the view's own joint, not a guessed parent index: an
     * intermediate helper between spine_1 and spine_2 used to break the whole
     * palette on the direct-parent check. Scan the anchored subtree above it.
     * A rig found from the movement transform has no spine index, so its own
     * root slot stands in as the subtree origin. */
    int spine=e->mesh_idx[4];
    if (spine<0 || spine>=count) spine=e->mesh_idx[0];
    if (spine<0 || spine>=count) { ++g_cham_fail; return; }
    /* Candidate subtree origins, tightest first. A rig found from the movement
     * transform treats its own slot as the character root, but that component
     * may point above the armature, and one hierarchy can hold several
     * characters. The scan therefore starts at the closest slot and widens one
     * ancestor at a time until a palette validates; the anchored path keeps
     * its single walk to the top, which is what the spine anchor implies. */
    int roots[10], root_count=0;
    for (int r=spine; root_count<10; ) {
        roots[root_count++]=r;
        if (!e->mesh_root) {
            for (int depth=0;depth<128;++depth) {
                int parent=parents[r];
                if (parent<0 || parent>=count || parent==r) break;
                r=parent;
            }
        }
        if (!e->mesh_root) break;
        int parent=parents[r];
        if (parent<0 || parent>=count || parent==r) break;
        r=parent;
    }
    if (previous && previous->cham_ready && previous->mesh_native[0]==e->mesh_native[0]) {
        int valid=1;
        for (int b=0;b<RR_CHAM_BONES;++b) {
            int idx=previous->cham_idx[b];
            if(idx<0) { if(rr_rig_required_joint(b)) valid=0; continue; }
            if (idx>=count || !PL(previous->cham_native[b]) || slots[idx]!=previous->cham_native[b] ||
                !native_joint_belongs(slots[idx],h,idx)) { valid=0; break; }
        }
        if (valid && rr_rig_mapping_valid(previous->cham_idx,parents,count)) {
            memcpy(e->cham_idx,previous->cham_idx,sizeof(e->cham_idx));
            memcpy(e->cham_native,previous->cham_native,sizeof(e->cham_native));
            cache_mesh_span(e, parents, count);
            e->cham_ready=1;
            ++g_cham_ok;
            return;
        }
    }
    int root=roots[root_count-1];
    for (int ci=0; ci<root_count; ++ci) {
        root=roots[ci];
        for (int b=0;b<RR_CHAM_BONES;++b) map[b]=-1;
        /* Seed the five view joints but keep them only if their native name agrees;
         * the name scan below then fills the rest of the body. */
        for (int b=0;b<5;++b) {
            int idx=e->mesh_idx[4-b];
            if (idx<0 || idx>=count || !PL(slots[idx]) || !native_joint_belongs(slots[idx],h,idx)) continue;
            uint64_t object=rr_u64(slots[idx]+0x20);
            char name[128];
            if (!PL(object) || !ascii_at(rr_u64(object+0x50),name,sizeof(name))) continue;
            if (!strcmp(name,rr_cham_bones[b].name)) map[b]=idx;
        }
        int ambiguous=0;
        for (int i=0;i<count && !ambiguous;++i) {
            if (!PL(slots[i]) || !rr_rig_descendant(parents,count,i,root) || !native_joint_belongs(slots[i],h,i)) continue;
            uint64_t object=rr_u64(slots[i]+0x20);
            char name[128];
            if (!PL(object) || !ascii_at(rr_u64(object+0x50),name,sizeof(name))) continue;
            for (int b=0;b<RR_CHAM_BONES;++b) {
                if (strcmp(name,rr_cham_bones[b].name)) continue;
                if(map[b]==-2)break;
                if (map[b]>=0 && map[b]!=i) {
                    if (g_pose_diag_enabled) { ++g_pose_diag.mapping_ambiguous; ++g_pose_diag.ambiguous_by_bone[b]; }
                    if(rr_rig_required_joint(b)) { ambiguous=1; break; }
                    map[b]=-2;break;
                }
                map[b]=i;
                if (g_pose_diag_enabled) ++g_pose_diag.mapping_joints;
                break;
            }
        }
        if (ambiguous) continue;
        for(int b=0;b<RR_CHAM_BONES;++b)if(map[b]<0)map[b]=-1;
        rr_rig_prune_optional(map,parents,count);
        if (!rr_rig_mapping_valid(map,parents,count)) continue;
        diag_validate_bone_names(map,slots);
        for (int b=0;b<RR_CHAM_BONES;++b) { e->cham_idx[b]=map[b]; e->cham_native[b]=map[b]>=0?slots[map[b]]:0; }
        cache_mesh_span(e, parents, count);
        e->cham_ready=1;
        ++g_cham_ok;
        return;
    }
    ++g_cham_fail;
    log_rig_mapping_failure(e,map,slots,count,root);
}

static void cham_matrix_multiply(const float a[12], const float b[12], float out[12]) {
    for (int r=0;r<3;++r) {
        for (int c=0;c<3;++c)
            out[r*4+c]=a[r*4]*b[c]+a[r*4+1]*b[4+c]+a[r*4+2]*b[8+c];
        out[r*4+3]=a[r*4]*b[3]+a[r*4+1]*b[7]+a[r*4+2]*b[11]+a[r*4+3];
    }
}

static int cham_local_matrix(const MESH_TRS* t, float m[12]) {
    float x=t->q[0],y=t->q[1],z=t->q[2],w=t->q[3], sx=t->s[0],sy=t->s[1],sz=t->s[2];
    float norm=x*x+y*y+z*z+w*w;
    if (!isfinite(norm) || norm<0.25f || norm>2.25f || !isfinite(sx) || !isfinite(sy) || !isfinite(sz) ||
        !isfinite(t->p[0]) || !isfinite(t->p[1]) || !isfinite(t->p[2])) return 0;
    m[0]=(1-2*(y*y+z*z))*sx; m[1]=2*(x*y-z*w)*sy; m[2]=2*(x*z+y*w)*sz; m[3]=t->p[0];
    m[4]=2*(x*y+z*w)*sx; m[5]=(1-2*(x*x+z*z))*sy; m[6]=2*(y*z-x*w)*sz; m[7]=t->p[1];
    m[8]=2*(x*z-y*w)*sx; m[9]=2*(y*z+x*w)*sy; m[10]=(1-2*(x*x+y*y))*sz; m[11]=t->p[2];
    return 1;
}

static int vehicle_cham_matrix(const MESH_TRS* chain, int depth, float matrix[12]) {
    if (depth < 1 || depth > RR_VEHICLE_POSE_DEPTH) return 0;
    for (int i = depth - 1; i >= 0; --i) {
        MESH_TRS trs = chain[i];
        float norm = 0.0f;
        for (int axis = 0; axis < 4; ++axis) norm += trs.q[axis] * trs.q[axis];
        if (!(norm >= 0.9f && norm <= 1.1f)) return 0;
        float inverse_norm = 1.0f / sqrtf(norm);
        for (int axis = 0; axis < 4; ++axis) trs.q[axis] *= inverse_norm;
        float local[12];
        if (!cham_local_matrix(&trs, local)) return 0;
        if (i == depth - 1) memcpy(matrix, local, sizeof(local));
        else {
            float combined[12];
            cham_matrix_multiply(matrix, local, combined);
            memcpy(matrix, combined, sizeof(combined));
        }
    }
    return fabsf(matrix[3]) <= 500000.0f && fabsf(matrix[7]) <= 500000.0f &&
           fabsf(matrix[11]) <= 500000.0f;
}

typedef struct { int valid; float matrix[12]; } VEH_CHAM_POSE;
typedef struct {
    uint64_t world, object, links[RR_VEHICLE_POSE_DEPTH];
    int id, depth;
    DWORD valid_at;
} VEH_CHAM_KEY;

static void read_vehicle_pose_batch(RR_RD_REQ* requests, int count) {
    if (g_batch_state >= 0 && rr_read_batch(requests, count) >= 0) {
        g_batch_state = 1;
        return;
    }
    if (g_batch_state == 0) g_batch_state = -1;
    for (int i = 0; i < count; ++i) {
        MESH_TRS* trs = requests[i].dst;
        if (!rr_read(requests[i].addr, trs, sizeof(*trs))) trs->q[3] = NAN;
    }
}

static const VEH_CHAM_POSE* sample_vehicle_poses(const VEH_CACHE* vehicles, int count,
                                                 unsigned generation, DWORD now) {
    static VEH_CHAM_POSE poses[RR_MAX_VEHICLES];
    static MESH_TRS chains[RR_MAX_VEHICLES][RR_VEHICLE_POSE_DEPTH];
    static VEH_CHAM_KEY keys[RR_MAX_VEHICLES];
    static unsigned last_generation;
    static DWORD last_sample;
    if (!g_pose_clock_changed && last_generation == generation &&
        now - last_sample < (g_batch_state < 0 ? 8u : 4u)) return poses;
    last_generation = generation;
    last_sample = now;

    RR_RD_REQ requests[BATCH_MAX];
    int requested = 0;
    for (int i = 0; i < count; ++i) {
        for (int link = 0; link < vehicles[i].render_depth; ++link) {
            MESH_TRS* trs = &chains[i][link];
            trs->q[3] = NAN;
            requests[requested++] = (RR_RD_REQ){ vehicles[i].render_trs[link], sizeof(*trs), trs };
            if (requested == BATCH_MAX) {
                read_vehicle_pose_batch(requests, requested);
                requested = 0;
            }
        }
    }
    if (requested) read_vehicle_pose_batch(requests, requested);
    for (int i = 0; i < count; ++i) {
        const VEH_CACHE* vehicle = &vehicles[i];
        VEH_CHAM_KEY* key = &keys[i];
        int same_pose = key->world == g_world && key->object == vehicle->obj && key->id == vehicle->id &&
            key->depth == vehicle->render_depth && vehicle->render_depth > 0 &&
            memcmp(key->links, vehicle->render_trs, vehicle->render_depth * sizeof(key->links[0])) == 0;
        float matrix[12];
        if (vehicle_cham_matrix(chains[i], vehicle->render_depth, matrix)) {
            DWORD elapsed = now - key->valid_at;
            if (same_pose && poses[i].valid && elapsed > 0 && elapsed < 100) {
                float dx=matrix[3]-poses[i].matrix[3], dy=matrix[7]-poses[i].matrix[7];
                float dz=matrix[11]-poses[i].matrix[11];
                if (dx*dx+dy*dy+dz*dz < 4.0f) {
                    float blend=(float)elapsed/((float)elapsed+3.0f);
                    for (int c=0;c<12;++c)
                        matrix[c]=poses[i].matrix[c]+(matrix[c]-poses[i].matrix[c])*blend;
                }
            }
            memcpy(poses[i].matrix, matrix, sizeof(matrix));
            poses[i].valid = 1;
            key->world = g_world;
            key->object = vehicle->obj;
            key->id = vehicle->id;
            key->depth = vehicle->render_depth;
            memcpy(key->links, vehicle->render_trs, key->depth * sizeof(key->links[0]));
            key->valid_at = now;
        } else if (!same_pose || now - key->valid_at > 80) {
            poses[i].valid = 0;
        }
    }
    return poses;
}

static int cham_world_matrix(int index, int count, const MESH_TRS* trs, const int* parents,
                             float world[][12], uint8_t* ready) {
    int path[128], length=0, i=index;
    while (i>=0 && i<count && !ready[i]) {
        if (length>=128) return 0;
        for (int k=0;k<length;++k) if (path[k]==i) return 0;
        path[length++]=i; i=parents[i];
    }
    if (i!=-1 && (i<0 || i>=count)) return 0;
    while (length) {
        int node=path[--length], parent=parents[node]; float local[12];
        if (!cham_local_matrix(&trs[node],local)) return 0;
        if (parent<0) memcpy(world[node],local,sizeof(local));
        else { if (!ready[parent]) return 0; cham_matrix_multiply(world[parent],local,world[node]); }
        ready[node]=1;
    }
    return 1;
}

static int build_cham_skin(const ENT_CACHE* e, RR_PLAYER* p, const MESH_TRS* trs,
                            const int* parents, const uint64_t* slots, int first, int count) {
    if (!e->cham_ready) return 0;
    float world[2048][12]; uint8_t ready[2048]={0};
    for (int b=0;b<RR_CHAM_BONES;++b) {
        for(int k=0;k<3;++k)p->bone[b][k]=NAN;
        for(int k=0;k<12;++k)p->cham_skin[b][k]=NAN;
        int idx=e->cham_idx[b]-first;
        if (idx<0 || idx>=count || !PL(e->cham_native[b]) || slots[idx]!=e->cham_native[b] ||
            !cham_world_matrix(idx,count,trs,parents,world,ready)) {
            if(rr_rig_required_joint(b))return 0;continue;
        }
        float dx=world[idx][3]-p->pos.x, dy=world[idx][7]-p->pos.y, dz=world[idx][11]-p->pos.z;
        if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz) || dx*dx+dy*dy+dz*dz>36.0f) {
            if(rr_rig_required_joint(b))return 0;continue;
        }
        float skin[12]; int valid=1;
        cham_matrix_multiply(world[idx],rr_cham_bones[b].bind,skin);
        for (int component=0;component<12;++component)
            if (!isfinite(skin[component])) valid=0;
        if(!valid){if(rr_rig_required_joint(b))return 0;continue;}
        memcpy(p->cham_skin[b],skin,sizeof(skin));
        p->bone[b][0]=world[idx][3];
        p->bone[b][1]=world[idx][7];
        p->bone[b][2]=world[idx][11];
    }
#ifdef RR_INTERNAL
    p->render_anchor = e->cham_native[RR_RIG_M_SPINE_1_JNT];
    p->render_hierarchy = e->mesh_h;
#endif
    p->nbones=RR_CHAM_BONES; p->full_rig=1;
    p->bone_group[0]=RR_RIG_M_HEAD_1_JNT; p->bone_group[1]=RR_RIG_M_NECK_JNT;
    p->bone_group[2]=RR_RIG_M_SPINE_3_JNT; p->bone_group[3]=RR_RIG_M_SPINE_1_JNT;
    p->cham_valid=1; return 1;
}

static void reuse_cham_pose(const ENT_CACHE* e, const MOTION* m, RR_PLAYER* p,
                            RR_V3 origin, DWORD now) {
    if (!m->mesh_cham_valid || now - m->cham_time >= 100 ||
        (e->mesh_h && (m->mesh_h != e->mesh_h || m->mesh_head != e->mesh_native[0])) ||
        (e->cham_ready && m->cham_root != e->cham_native[0])) return;
    float dx = origin.x - m->cham_origin.x;
    float dy = origin.y - m->cham_origin.y;
    float dz = origin.z - m->cham_origin.z;
    if (dx*dx + dy*dy + dz*dz >= 0.25f) return;
#ifdef RR_INTERNAL
    p->render_anchor = e->cham_native[RR_RIG_M_SPINE_1_JNT];
    p->render_hierarchy = e->mesh_h;
#endif
    memcpy(p->cham_skin, m->cham_skin, sizeof(p->cham_skin));
    p->cham_sample_at = m->cham_time;
    p->cham_rig = m->cham_root;
    p->cham_valid = 1;
}

/* A single failed mesh read must not jump the box and skeleton back to the
 * replicated transform while the previous rendered pose is still valid. The
 * mapping-ready flag is deliberately not part of the match: a palette that is
 * being re-validated this pass has not invalidated the native joints. */
static int reuse_mesh_pose(const ENT_CACHE* e, const MOTION* m, RR_PLAYER* p,
                           RR_V3 origin, DWORD now) {
    if (!m->mesh_valid || m->mesh_nbones<1 || m->mesh_nbones>RR_MAX_BONES ||
        now - m->mesh_time >= 150 || !PL(e->mesh_h) ||
        m->mesh_h != e->mesh_h || m->mesh_head != e->mesh_native[0] ||
        (e->cham_ready && m->cham_root != e->cham_native[0])) return 0;
    float dx = origin.x - m->mesh_origin.x, dy = origin.y - m->mesh_origin.y;
    float dz = origin.z - m->mesh_origin.z;
    if (dx*dx + dy*dy + dz*dz >= 4.0f) return 0;
    memcpy(p->bone, m->mesh_bone, sizeof(m->mesh_bone));
    p->nbones=m->mesh_nbones; p->mesh_bones=1; p->full_rig=m->mesh_full_rig;
    memcpy(p->bone_group,m->mesh_group,sizeof(p->bone_group));
    if (p->full_rig) { p->pos=m->mesh_feet; p->head=m->mesh_top; }
#ifdef RR_INTERNAL
    /* Keep the previous articulation for a short gap, but carry it with
       the measured model movement so the rig does not freeze then jump. */
    for (int bone = 0; bone < p->nbones; ++bone) {
        p->bone[bone][0] += dx;
        p->bone[bone][1] += dy;
        p->bone[bone][2] += dz;
    }
    if (p->full_rig) {
        p->render_anchor = e->cham_native[RR_RIG_M_SPINE_1_JNT];
        p->render_hierarchy = e->mesh_h;
        p->pos.x += dx; p->pos.y += dy; p->pos.z += dz;
        p->head.x += dx; p->head.y += dy; p->head.z += dz;
    }
#endif
    return 1;
}

static float diag_quat_similarity(const float a[4],const float b[4]) {
    float dot=0, aa=0, bb=0;
    for (int i=0;i<4;++i) { dot+=a[i]*b[i]; aa+=a[i]*a[i]; bb+=b[i]*b[i]; }
    if (!isfinite(dot) || aa<0.25f || bb<0.25f) return 0;
    return fabsf(dot)/sqrtf(aa*bb);
}

/* Diagnostic only: compare local bone rotations, before bind matrices or
 * smoothing. Also compare two immediate reads of the same native slots. */
static void diagnose_local_rotations(const ENT_CACHE* e, MOTION* motion,
                                     const MESH_TRS* trs, int first, int count,
                                     uint64_t matrices) {
    if (!g_pose_diag_enabled || !motion || !e->cham_ready) return;
    const int bone[3]={RR_RIG_M_NECK_JNT,RR_RIG_L_ARM_1_JNT,RR_RIG_R_ARM_1_JNT};
    float q[3][4]; RR_RD_REQ reads[3]; MESH_TRS again[3];
    for (int i=0;i<3;++i) {
        int index=e->cham_idx[bone[i]]-first;
        if (index<0 || index>=count) return;
        memcpy(q[i],trs[index].q,sizeof(q[i]));
        reads[i]=(RR_RD_REQ){matrices+(uint64_t)e->cham_idx[bone[i]]*sizeof(MESH_TRS),sizeof(MESH_TRS),&again[i]};
    }
    ++g_pose_diag.quat_samples;
    const float rest_arm[4]={.0322450772f,-.0249513127f,.499164045f,.865547776f};
    int both_rest=diag_quat_similarity(q[1],rest_arm)>.9990f && diag_quat_similarity(q[2],rest_arm)>.9990f;
    if (both_rest) ++g_pose_diag.both_arms_rest;
    if (motion->diag_quat_count>=2 &&
        diag_quat_similarity(motion->diag_quat[0][1],rest_arm)>.9990f &&
        diag_quat_similarity(motion->diag_quat[0][2],rest_arm)>.9990f &&
        !both_rest && diag_quat_similarity(q[1],motion->diag_quat[1][1])>.9962f &&
        diag_quat_similarity(q[2],motion->diag_quat[1][2])>.9962f)
        ++g_pose_diag.rest_reversions;
    if (motion->diag_quat_count>=2) for (int i=0;i<3;++i) {
        if (diag_quat_similarity(q[i],motion->diag_quat[0][i]) < .9763f &&
            diag_quat_similarity(q[i],motion->diag_quat[1][i]) > .9962f) {
            if (i==0) ++g_pose_diag.neck_reversals;
            else if (i==1) ++g_pose_diag.left_reversals;
            else ++g_pose_diag.right_reversals;
        }
    }
    for (int axis=0;axis<3;++axis) {
        int available=motion->diag_rotation_count;
        for (int length=1;length<=6 && length<available;++length) {
            if (diag_quat_similarity(q[axis],motion->diag_rotation_history[axis][length])<.9962f) continue;
            int apart=1;
            for (int j=0;j<length;++j)
                if (diag_quat_similarity(q[axis],motion->diag_rotation_history[axis][j])>=.9763f) { apart=0; break; }
            if (apart) { ++g_pose_diag.rotation_run_return[length-1]; break; }
        }
        for (int j=7;j>0;--j)
            memcpy(motion->diag_rotation_history[axis][j],motion->diag_rotation_history[axis][j-1],sizeof(q[axis]));
        memcpy(motion->diag_rotation_history[axis][0],q[axis],sizeof(q[axis]));
    }
    if (motion->diag_rotation_count<8) ++motion->diag_rotation_count;
    memcpy(motion->diag_quat[1],motion->diag_quat[0],sizeof(motion->diag_quat[0]));
    memcpy(motion->diag_quat[0],q,sizeof(q));
    if (motion->diag_quat_count<2) ++motion->diag_quat_count;
    if ((g_pose_diag.quat_samples & 7u)==0 && rr_read_batch(reads,3)==3) {
        ++g_pose_diag.pair_samples;
        for (int i=0;i<3;++i) if (diag_quat_similarity(q[i],again[i].q)<.9763f) {
            ++g_pose_diag.pair_changes; break;
        }
    }
}


static void filter_arm_branches(const ENT_CACHE* e,MOTION* motion,MESH_TRS* trs,int first,int count,
                                DWORD now) {
    if(!motion || !e->cham_ready)return;
    int reset=!motion->mesh_valid || motion->mesh_h!=e->mesh_h || motion->mesh_head!=e->mesh_native[0] ||
              motion->cham_root!=e->cham_native[0] || now-motion->mesh_time>=100;
    const int chain_parents[4]={-1,0,1,2};
    MESH_TRS torso[4];float torso_q[4][4];int valid_torso=1;
    for(int b=0;b<4;++b) {
        int index=e->cham_idx[b]-first;
        if(index<0 || index>=count){valid_torso=0;break;}
        torso[b]=trs[index];memcpy(torso_q[b],torso[b].q,sizeof(torso_q[b]));
    }
    RR_V3 torso_tip;
    if(valid_torso && mesh_point(torso,chain_parents,4,3,&torso_tip)) {
        int hold=rr_pose_gate_hold(&motion->torso_gate,torso_q,4,&torso_tip.x,now,reset);
        for(int b=0;b<4;++b) {
            if(hold)trs[e->cham_idx[b]-first]=motion->torso_trs[b];
            else motion->torso_trs[b]=torso[b];
        }
    }
    /* Head rotation is outside the spine/neck chain. Hold its facial subtree
       with it, so the face never mixes a transient head with accepted bones. */
    int head_index=e->cham_idx[RR_RIG_M_HEAD_1_JNT]-first;
    if(head_index>=0 && head_index<count) {
        float head_q[4][4]={0};memcpy(head_q[0],trs[head_index].q,sizeof(head_q[0]));
        int hold=rr_pose_gate_hold(&motion->head_gate,head_q,1,trs[head_index].p,now,reset);
        for(int b=RR_RIG_M_HEAD_1_JNT;b<RR_RIG_L_ARM_0_JNT;++b) {
            int index=e->cham_idx[b]-first,saved=b-RR_RIG_M_HEAD_1_JNT;
            if(index<0 || index>=count)continue;
            if(hold)trs[index]=motion->head_trs[saved];
            else motion->head_trs[saved]=trs[index];
        }
    }
    const int starts[2]={RR_RIG_L_ARM_0_JNT,RR_RIG_R_ARM_0_JNT};
    const int ends[2]={RR_RIG_L_ARMTWIST_JNT,RR_RIG_R_ARMTWIST_JNT};
    for(int side=0;side<2;++side) {
        float q[4][4];MESH_TRS branch[4];int valid=1;
        for(int j=0;j<4;++j) {
            int index=e->cham_idx[starts[side]+j]-first;
            if(index<0 || index>=count) {valid=0;break;}
            memcpy(q[j],trs[index].q,sizeof(q[j]));
            branch[j]=trs[index];
        }
        if(!valid)continue;
        RR_V3 tip;
        if(!mesh_point(branch,chain_parents,4,3,&tip))continue;
        int hold=rr_pose_gate_hold(&motion->arm_gate[side],q,4,&tip.x,now,reset);
        for(int b=starts[side];b<=ends[side];++b) {
            int index=e->cham_idx[b]-first, saved=b-RR_RIG_L_ARM_0_JNT;
            if(index<0 || index>=count)continue;
            if(hold) trs[index]=motion->arm_trs[saved];
            else motion->arm_trs[saved]=trs[index];
        }
        if(hold && g_pose_diag_enabled)++g_pose_diag.arm_branch_holds[side];
    }
}

static int build_mesh_bones(const ENT_CACHE* e, RR_PLAYER* p, DWORD now) {
    static int mesh_batch_state;
    if (!PL(e->mesh_h)) return 0;
    const BR_Config* config = br_get();
    int use_cache=needs_mesh_pose(config, p->inveh);
#ifndef RR_INTERNAL
    use_cache = use_cache && !p->local;
#endif
    MOTION* motion=use_cache ? motion_slot(e->obj) : NULL;
    RR_V3 origin=p->pos;
    if (!p->local && motion && motion->mesh_valid && motion->mesh_h==e->mesh_h &&
        motion->mesh_head==e->mesh_native[0] && motion->mesh_cham_ready==e->cham_ready &&
        (!e->cham_ready || motion->cham_root==e->cham_native[0]) &&
        motion->mesh_nbones>0 && motion->mesh_nbones<=RR_MAX_BONES &&
        !g_pose_clock_changed && now-motion->mesh_time<4
#ifdef RR_INTERNAL
        && !p->inveh
#endif
        ) {
        float dx=origin.x-motion->mesh_origin.x, dy=origin.y-motion->mesh_origin.y, dz=origin.z-motion->mesh_origin.z;
        if (dx*dx+dy*dy+dz*dz<0.25f) {
            memcpy(p->bone, motion->mesh_bone, sizeof(motion->mesh_bone));
            p->nbones=motion->mesh_nbones; p->mesh_bones=1; p->full_rig=motion->mesh_full_rig;
            memcpy(p->bone_group,motion->mesh_group,sizeof(p->bone_group));
            if (p->full_rig) { p->pos=motion->mesh_feet; p->head=motion->mesh_top; }
            reuse_cham_pose(e, motion, p, origin, now);
            return 1;
        }
    }
    if (motion && (motion->mesh_h!=e->mesh_h || motion->mesh_head!=e->mesh_native[0])) {
        motion->mesh_valid=0;
        motion->mesh_cham_valid=0;
        motion->diag_quat_count=0;
        motion->diag_rotation_count=0;
        motion->cham_hold_pending=0;
    }
    if (motion && e->cham_ready && motion->cham_root!=e->cham_native[0]) {
        motion->mesh_cham_valid=0;
        motion->diag_quat_count=0;
        motion->diag_rotation_count=0;
        motion->cham_hold_pending=0;
    }
    /* +0x10 is the allocated array size; +0x38 is a free-list index and can
     * be smaller than a valid index or -1. Verify each slot's native pointer. */
    uint8_t header[0x28]; int count=0; uint64_t matrices=0,parents=0,pointers=0;
    if (!rr_read(e->mesh_h+0x10,header,sizeof(header))) return 0;
    memcpy(&count,header,4); memcpy(&matrices,header+8,8);
    memcpy(&parents,header+0x10,8); memcpy(&pointers,header+0x20,8);
    if (count <= 0 || count > 2048 || !PL(matrices) || !PL(parents) || !PL(pointers)) return 0;
    int first = e->mesh_span > 0 ? e->mesh_first : 0;
    if (e->mesh_span > 0) {
        if (first < 0 || first >= count || e->mesh_span > count - first) return 0;
        count = e->mesh_span;
    }
    MESH_TRS trs[2048]; int hierarchy[2048]; uint64_t slots[2048]; RR_V3 pt[5];
    RR_RD_REQ reads[] = {
        {matrices + (uint64_t)first * sizeof(trs[0]), count * sizeof(trs[0]), trs},
        {parents + (uint64_t)first * sizeof(hierarchy[0]), count * sizeof(hierarchy[0]), hierarchy},
        {pointers + (uint64_t)first * sizeof(slots[0]), count * sizeof(slots[0]), slots},
    };
    int copied = -1;
    if (mesh_batch_state >= 0 && reads[0].size + reads[1].size + reads[2].size <= 0xF000) {
        copied = rr_read_batch(reads, (int)_countof(reads));
        if (copied >= 0 && copied != _countof(reads)) return 0;
        if (copied >= 0) mesh_batch_state = 1;
        else if (!mesh_batch_state) mesh_batch_state = -1;
    }
    if (copied < 0) for (size_t i = 0; i < _countof(reads); ++i) {
        if (!read_mesh_array(reads[i].addr, reads[i].dst, reads[i].size)) return 0;
    }
    for (int i = 0; i < count; ++i) {
        int parent = hierarchy[i];
        if (parent != -1) hierarchy[i] = parent >= first && parent < first + count ? parent - first : -2;
    }
    diagnose_local_rotations(e,motion,trs,first,count,matrices);
#ifdef RR_CAPTURE_LOCAL
    RR_CAPTURE_LOCAL(e,trs,first,count,now);
#endif
    filter_arm_branches(e,p->local ? NULL : motion,trs,first,count,now);
    /* A root-only entry (mesh_from_root_transform) has no head/torso anchors;
     * the palette scan still seeds from the root slot, so the cham rig can be
     * built without them. The 7-bone chain is the only thing that truly needs
     * all five, and it is the fallback for a rig with no palette. */
    int anchors = 0;
    for (int i = 0; i < 5; ++i) {
        if (e->mesh_idx[i] < 0) continue;
        int index = e->mesh_idx[i] - first;
        if (index < 0 || index >= count || !PL(e->mesh_native[i]) || slots[index] != e->mesh_native[i]) return 0;
        if (!mesh_point(trs, hierarchy, count, index, &pt[i])) return 0;
        if (fabsf(pt[i].x-p->pos.x)>2 || fabsf(pt[i].y-p->pos.y)>3 || fabsf(pt[i].z-p->pos.z)>2) return 0;
        ++anchors;
    }
    RR_V3 chain[7];
    if (anchors == 5) {
        chain[0]=pt[4]; chain[1]=pt[4]; chain[2]=pt[3]; chain[3]=pt[2];
        chain[4]=pt[1]; chain[5]=pt[0]; chain[6]=pt[0];
        chain[1].y += p->crouch == 1 ? 0.48f : 0.88f; /* pelvis estimate */
        chain[6].y += 0.10f; /* top of head estimate */
    }
    if (build_cham_skin(e,p,trs,hierarchy,slots,first,count)) {
        /* full_rig + cham_valid are set inside */
    } else if (anchors == 5) {
        memcpy(p->bone, chain, sizeof chain);
        p->nbones=7;
        p->full_rig=0;
        p->bone_group[0]=5; p->bone_group[1]=4; p->bone_group[2]=3; p->bone_group[3]=1;
    } else {
        /* No palette and no anchors: let the skel path have the player back
         * rather than stamping a mesh_bones flag over an empty pose. */
        return 0;
    }
    p->mesh_bones=1;
    if (p->cham_valid) p->cham_sample_at=now;
    int top_bone=isfinite(p->bone[RR_RIG_M_HEAD_2_JNT][0]) ? RR_RIG_M_HEAD_2_JNT : RR_RIG_M_HEAD_1_JNT;
    RR_V3 top=p->full_rig ? (RR_V3){p->bone[top_bone][0],p->bone[top_bone][1],p->bone[top_bone][2]}
                          : (anchors == 5 ? chain[6] : p->head);
    const RR_V3 feet = anchors == 5 ? pt[4] : p->pos;
    if (motion) {
        if (p->cham_valid) {
            float hx=origin.x-motion->cham_origin.x, hy=origin.y-motion->cham_origin.y, hz=origin.z-motion->cham_origin.z;
            if (hx*hx+hy*hy+hz*hz>.0225f) motion->cham_hold_pending=0;
            if (g_pose_diag_enabled && !p->local) {
                if (p->inveh) {
                    ++g_pose_diag.seated_samples;
                    if (hx*hx+hy*hy+hz*hz>.0225f) ++g_pose_diag.seated_root_bypass;
                } else ++g_pose_diag.standing_samples;
            }
            if (g_pose_diag_enabled && motion->mesh_cham_valid && motion->mesh_valid && motion->mesh_h==e->mesh_h &&
                motion->mesh_head==e->mesh_native[0] && motion->cham_root==e->cham_native[0] &&
                hx*hx+hy*hy+hz*hz<=.0225f && now-motion->cham_time<100) {
                const int bones[3]={RR_RIG_M_HEAD_1_JNT,RR_RIG_L_ARM_1_JNT,RR_RIG_R_ARM_1_JNT}; int mask=0;
                for (int b=0;b<3;++b) {
                    float sum=0;
                    for(int row=0;row<3;++row) for(int col=0;col<3;++col) {
                        int idx=row*4+col; float v=p->cham_skin[bones[b]][idx]-motion->cham_skin[bones[b]][idx];sum+=v*v;
                    }
                    if (sum>.16f) mask|=1<<b;
                }
                ++g_pose_diag.change_mask[mask];
            }
            if (!p->local &&
#ifdef RR_INTERNAL
                !p->inveh &&
#endif
                motion->mesh_cham_valid && motion->mesh_valid && motion->mesh_h==e->mesh_h &&
                motion->mesh_head==e->mesh_native[0] && motion->cham_root==e->cham_native[0] &&
                hx*hx+hy*hy+hz*hz<=.0225f &&
                rr_cham_coherence_hold(motion->cham_skin,p->cham_skin,motion->cham_yaw,p->rot_yaw,
                                       now,now-motion->cham_time,&motion->cham_hold_since,&motion->cham_hold_pending)) {
                if (g_pose_diag_enabled) { ++g_pose_diag.held_spikes; if (p->inveh) ++g_pose_diag.seated_holds; }
                p->cham_valid=0; p->nbones=0; p->full_rig=0; p->mesh_bones=0;
                return 0;
            }
            p->cham_sample_at = now;
            motion->cham_root = e->cham_native[0];
            motion->cham_time = now;
            motion->cham_origin = origin;
            motion->cham_yaw = p->rot_yaw;
            motion->mesh_cham_valid = 1;
            memcpy(motion->cham_skin,p->cham_skin,sizeof(motion->cham_skin));
        }
        if (!p->full_rig && motion->mesh_valid && motion->mesh_full_rig &&
            now-motion->mesh_time < 150) return 1;
        motion->mesh_h=e->mesh_h; motion->mesh_head=e->mesh_native[0];
        motion->mesh_time=now; motion->mesh_origin=origin; motion->mesh_cham_ready=e->cham_ready;
        motion->mesh_nbones=p->nbones; motion->mesh_full_rig=p->full_rig;
        memcpy(motion->mesh_group,p->bone_group,sizeof(motion->mesh_group));
        motion->mesh_feet=feet; motion->mesh_top=top;
        memcpy(motion->mesh_bone,p->bone,sizeof(motion->mesh_bone));
        motion->mesh_valid=1;
    }
    if (p->full_rig) { p->pos=feet; p->head=top; }
    return 1;
}

#ifdef RR_PORT_DIAGNOSTICS
static int vehicle_marker_position(const VEH_CACHE* v, const RR_V3* model, RR_V3* out) {
    if (!PL(v->marker_transform) || !PL(v->marker_native) || !PL(v->marker_h) ||
        rr_u64(v->marker_transform+0x10)!=v->marker_native) return 0;
    uint8_t native_fields[12], header[0x28]; uint64_t h=0, matrices=0,parents=0,pointers=0;
    int capacity=0,idx=-1;
    if (!rr_read(v->marker_native+0x28,native_fields,sizeof(native_fields))) return 0;
    memcpy(&h,native_fields,8); memcpy(&idx,native_fields+8,4);
    if (h!=v->marker_h || !rr_read(h+0x10,header,sizeof(header))) return 0;
    memcpy(&capacity,header,4); memcpy(&matrices,header+8,8);
    memcpy(&parents,header+0x10,8); memcpy(&pointers,header+0x20,8);
    if (capacity<1 || capacity>2048 || idx!=v->marker_idx || idx<0 || idx>=capacity ||
        !PL(matrices) || !PL(parents) || !PL(pointers) || rr_u64(pointers+(uint64_t)idx*8)!=v->marker_native) return 0;
    MESH_TRS trs[2048]; int hierarchy[2048]; RR_V3 pos;
    if (!read_mesh_array(matrices,trs,(size_t)capacity*sizeof(trs[0])) ||
        !read_mesh_array(parents,hierarchy,(size_t)capacity*sizeof(hierarchy[0])) ||
        !mesh_point(trs,hierarchy,capacity,idx,&pos)) return 0;
    float x=pos.x-model->x,y=pos.y-model->y,z=pos.z-model->z;
    if (x*x+y*y+z*z>225.0f) return 0;
    *out=pos; return 1;
}

/* Camera. */
#endif
static int read_camera_from_local(RR_CAMERA* cam) {
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    int count = g_ncache;
    static ENT_CACHE loc[RR_MAX_PLAYERS];
    memcpy(loc, g_cache, sizeof(ENT_CACHE) * (size_t)count);
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    for (int i = 0; i < count; i++) {
        if (!loc[i].local) continue;
        float pos[3] = { 0, 0, 0 }, look[3] = { 0, 0, 0 };
        if (!rr_read(loc[i].tr + RR_OFF_CTM_X, pos, sizeof(pos))) return 0;
        uint64_t lm = rr_u64(loc[i].obj + RR_OFF_LOOK_MODEL);
        if (!PL(lm)) return 0;
        if (!rr_read(lm + 0x10, look, sizeof(look))) return 0;
        float len = sqrtf(look[0]*look[0] + look[1]*look[1] + look[2]*look[2]);
        if (len < 0.5f) return 0;
        look[0] /= len; look[1] /= len; look[2] /= len;
        /* Eye position, no lateral offset: the 2.5 m third-person pull-back made
         * the projection drift as the look direction changed. */
        cam->pos.x = pos[0];
        cam->pos.y = pos[1] + 1.60f;
        cam->pos.z = pos[2];
        cam->fwd.x = look[0]; cam->fwd.y = look[1]; cam->fwd.z = look[2];
        cam->rot.w = 1.0f;
        cam->fov = g_fov;
        cam->valid = 1;
        return 1;
    }
    return 0;
}

static int read_camera(RR_CAMERA* cam) {
    memset(cam, 0, sizeof(*cam));
    /* The real camera is third-person and sits on the player: use it whenever it
     * is near them. The local-eye fallback is first-person, which offsets every
     * marker, so it is only used when no plausible camera exists. */
    if (camera_near_local(g_cam_obj) == 1) {
        uint8_t buf[0x50];
        if (rr_read(g_cam_obj + RR_OFF_CAM_POS, buf, sizeof(buf))) {
            memcpy(&cam->pos, buf + 0x00, sizeof(RR_V3));
            memcpy(&cam->rot, buf + (RR_OFF_CAM_ROT - RR_OFF_CAM_POS), sizeof(RR_Q4));
            memcpy(&cam->fwd, buf + (RR_OFF_CAM_FWD - RR_OFF_CAM_POS), sizeof(RR_V3));
            memcpy(&cam->right, buf + (RR_OFF_CAM_RIGHT - RR_OFF_CAM_POS), sizeof(RR_V3));
            memcpy(&cam->up, buf + (RR_OFF_CAM_UP - RR_OFF_CAM_POS), sizeof(RR_V3));
            cam->fov = g_fov;
            /* CinemachineBrain.CurrentCameraState.Lens.FieldOfView is the
             * blended vertical FOV, including aiming and camera transitions. */
            uint64_t aim = g_aim_updater;
            if (PL(aim) && PL(g_battle) && rr_u64(g_battle+RR_OFF_BATTLE_WORLD)==g_world &&
                rr_u64(aim+RR_OFF_AIM_BATTLE)==g_battle && class_named(rr_u64(aim),"BattleAimTargetUpdater")) {
                uint64_t brain = rr_u64(aim+RR_OFF_AIM_BRAIN);
                float fov = 0; RR_V3 raw;
                if (PL(brain) && class_named(rr_u64(brain),"CinemachineBrain") &&
                    rr_read(brain+0xB0,&fov,sizeof(fov)) && rr_read(brain+0xFC,&raw,sizeof(raw)) &&
                    isfinite(fov) && fov>10.0f && fov<170.0f &&
                    isfinite(raw.x) && isfinite(raw.y) && isfinite(raw.z)) {
                    float dx=raw.x-cam->pos.x, dy=raw.y-cam->pos.y, dz=raw.z-cam->pos.z;
                    if (dx*dx+dy*dy+dz*dz < 25.0f) cam->fov=fov;
                }
            }
            cam->valid = 1;
            cam->model_source = g_cam_obj;
            return 1;
        }
    }
    return read_camera_from_local(cam);
}

int game_aim_target(int32_t character_id) {
    if (!PL(g_battle_model)) return 0;
    const uint64_t aim = rr_u64(g_battle_model + RR_OFF_BATTLE_AIM);
    if (!PL(aim) || !class_named(rr_u64(aim), "AimTargetModel")) return 0;
    const int32_t id = character_id > 0 ? character_id : 0;
    /* AimTargetModel.Type: 0 Undefined, 3 BattleCharacter. */
    const int32_t type = id ? 3 : 0;
    int ok = rr_write(aim + RR_OFF_AIM_CHARACTER_ID, &id, sizeof id);
    ok &= rr_write(aim + RR_OFF_AIM_TARGET_TYPE, &type, sizeof type);
    return ok;
}

/* Silent aim: ShotArgsSystem builds the shot ray from the local
 * BattleCharacterModel.LookDirection (LookDirectionModel._value at 0x10) with
 * the camera as origin. ShotInterpolationModel lerps the previous direction
 * toward the current one across the tick, so a moving target keeps a one-tick
 * lead on whatever only _value carries. AimTargetModel stays reticle-only.
 *
 * The look tick rewrites _value from the mouse every frame, which is why a
 * single write per frame loses on the frame the shot is actually built. The
 * writer therefore pins all three copies the shot path can read, polls _value
 * at 4 kHz and re-states the direction the moment the game overwrites it, so
 * the mouse direction is exposed for well under a millisecond per frame. */
#define RR_OFF_CHAR_LOOK      0x70u   /* BattleCharacterModel.LookDirection */
#define RR_OFF_CHAR_LASTLOOK  0x88u   /* BattleCharacterModel.LastLookDirection */
#define RR_OFF_CHAR_SHOTINT   0x108u  /* BattleCharacterModel.ShotInterpolationModel */
#define RR_OFF_LOOK_VALUE     0x10u   /* LookDirectionModel._value */
#define RR_OFF_SHOT_LOOKDIR   0x1Cu   /* ShotInterpolationModel._lookDirection */
#define RR_OFF_LAST_DIR       0x18u   /* LastLookDirectionModel.Direction */
#define RR_SA_POLL_HZ         4000

static volatile LONG  g_sa_on;
static volatile float g_sa_pt[3];   /* silent aim target */
static volatile LONG  g_sa_thread;

typedef struct {
    uint64_t local, look, shotint, lastlook;
    DWORD    at;
} SA_CHAIN;

static SA_CHAIN g_sa_chain;
static RR_V3    g_sa_eye;
static DWORD    g_sa_eye_at;

static uint64_t sa_local(void) {
    uint64_t local = 0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    for (int i = 0; i < g_ncache; ++i) if (g_cache[i].local) { local = g_cache[i].obj; break; }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    return PL(local) ? local : 0;
}

/* Pointers only move on respawn, so the chain is re-read four times a second;
 * a local that vanished forces an immediate rebuild. Missing pieces are left
 * zero and each writer skips what it needs. */
static int sa_chain(SA_CHAIN* c) {
    const uint64_t local = sa_local();
    if (!local) return 0;
    const DWORD now = timeGetTime();
    if (local == c->local && now - c->at <= 250) return 1;
    c->local = local;
    c->at = now;
    c->look = c->shotint = c->lastlook = 0;

    const uint64_t look = rr_u64(local + RR_OFF_CHAR_LOOK);
    if (u_is(look, "LookDirectionModel")) {
        c->look = look;
        const uint64_t si = rr_u64(local + RR_OFF_CHAR_SHOTINT);
        const uint64_t ll = rr_u64(local + RR_OFF_CHAR_LASTLOOK);
        if (u_is(si, "ShotInterpolationModel")) c->shotint = si;
        if (u_is(ll, "LastLookDirectionModel")) c->lastlook = ll;
    }

    return 1;
}

/* Death cam, spectate and the frames right after a respawn all fail the
 * ownership test; the camera position from the last 250 ms is still the
 * shooter's, so the shot keeps lining up instead of dropping out. */
static int sa_eye(RR_V3* eye) {
    RR_V3 pos;
    if (camera_near_local(g_cam_obj) == 1 && rr_read(g_cam_obj + RR_OFF_CAM_POS, &pos, sizeof pos)) {
        g_sa_eye = pos;
        g_sa_eye_at = timeGetTime();
        *eye = pos;
        return 1;
    }
    if (timeGetTime() - g_sa_eye_at > 250) return 0;
    *eye = g_sa_eye;
    return 1;
}

static int silent_write(SA_CHAIN* c) {
    RR_V3 eye;
    if (!sa_eye(&eye)) return 0;
    const float dx = g_sa_pt[0] - eye.x, dy = g_sa_pt[1] - eye.y, dz = g_sa_pt[2] - eye.z;
    const float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (!(len > 0.5f) || !isfinite(len)) return 0;
    const float v[3] = {dx / len, dy / len, dz / len};

    /* Most polls land on the value we already own; only the frame where the
     * look tick rebuilt it costs writes. */
    float cur[3];
    if (rr_read(c->look + RR_OFF_LOOK_VALUE, cur, sizeof cur) &&
        cur[0]*v[0] + cur[1]*v[1] + cur[2]*v[2] > 0.9999f)
        return 1;
    int ok = rr_write(c->look + RR_OFF_LOOK_VALUE, v, sizeof v);
    if (c->shotint) rr_write(c->shotint + RR_OFF_SHOT_LOOKDIR, v, sizeof v);
    if (c->lastlook) rr_write(c->lastlook + RR_OFF_LAST_DIR, v, sizeof v);
    return ok;
}

static int sa_any(void) {
    return InterlockedCompareExchange(&g_sa_on, 0, 0) != 0;
}

static DWORD WINAPI silent_worker(LPVOID p) {
    (void)p;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const LONGLONG slot = freq.QuadPart / RR_SA_POLL_HZ;
    for (;;) {
        if (!sa_any()) { Sleep(10); continue; }
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        SA_CHAIN* c = &g_sa_chain;
        if (sa_chain(c)) silent_write(c);
        for (;;) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart - start.QuadPart >= slot) break;
            Sleep(0);   /* yields the rest of the slot, never spins hot */
        }
    }
}

static void silent_start(void) {
    if (InterlockedCompareExchange(&g_sa_thread, 1, 0) != 0) return;
    HANDLE th = rr_thread_start(silent_worker, NULL);
    if (th) CloseHandle(th);
    else InterlockedExchange(&g_sa_thread, 0);
}

void game_silent_aim(int on, RR_V3 target) {
    if (!on) { InterlockedExchange(&g_sa_on, 0); return; }
    g_sa_pt[0] = target.x;
    g_sa_pt[1] = target.y;
    g_sa_pt[2] = target.z;
    InterlockedExchange(&g_sa_on, 1);
    silent_start();
}

/* ---- camera lock / camera FOV ---- */
/* The lock turns the view through the model the rig reads:
 * BattleAimTargetUpdater._playerRotationInput (0x38) -> PlayerRotationInputModel
 * _yaw (0x10) / _pitch (0x14). That is a plain two-float input model, so the
 * writer reads the pair and the live camera once per tick and writes back the
 * model's own value plus the angular error. Units (degrees or radians) and per
 * axis sign are read once on engage by comparing the model with the camera
 * forward; there is no learning loop, no sensitivity mute and no sign flipping
 * after that. Smoothing is a first-order approach on the error; 0 is instant.
 * The 0xB0 Cinemachine CameraState stays out of this path: it is the brain's
 * per-frame output and an outside write there is overwritten before render. */
/* Each tick costs one batched read (camera block + rotation pair) and one
 * 8-byte yaw/pitch store. */
#define RR_CL_POLL_HZ 1000
#define RR_CL_RAD2DEG 57.2957795f
#define RR_CL_TAU_S 0.14f          /* base slew time constant, seconds */
/* The aiming pass feeds the point once per snapshot. Older than this means the
 * feed stopped (menu, focus loss, no target) and the writer must hold still
 * instead of dragging the camera at the last known spot. */
#define RR_CL_POINT_MAX_MS 150u

static volatile LONG  g_cl_on;
static volatile LONG  g_cl_thread;
static volatile LONG  g_cl_engaged;
static volatile float g_cl_pt[3];
static volatile DWORD g_cl_pt_at;      /* timeGetTime() when the point was fed */
static volatile float g_cl_fov;        /* 0 leaves the game's own FOV alone */
static volatile float g_cl_smooth = 1.0f;

/* Model units per camera degree and the sign of each axis, read once per
 * rotation model by comparing it against the live camera. A rotation input
 * model stores angles in degrees or radians, and a model can mirror one axis;
 * both are answered at engage, and every later write is a direct correction
 * of the model's own value against the camera's error. No learning loop, no
 * sign flipping, no sensitivity muting: if the write does not turn the view
 * the model is not the one the game reads, and no amount of tuning fixes it. */
static float g_cl_yaw_unit = 1.0f, g_cl_pitch_unit = 1.0f;
static float g_cl_yaw_sign = 1.0f, g_cl_pitch_sign = 1.0f;
static float g_cl_pt_s[3];             /* 15 ms low-passed aim point */
static int   g_cl_pt_s_valid;
static uint64_t g_cl_rot;

static float cl_wrap180(float d) {
    d = fmodf(d + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

static uint64_t cl_brain(void) {
    const uint64_t aim = g_aim_updater;
    if (!PL(aim)) return 0;
    const uint64_t brain = rr_u64(aim + RR_OFF_AIM_BRAIN);
    return PL(brain) && class_named(rr_u64(brain), "CinemachineBrain") ? brain : 0;
}

#ifdef RR_INTERNAL
typedef void (*RR_PUSH_CAMERA)(uint64_t, void*, void*);
typedef uint64_t (*RR_OUTPUT_CAMERA)(uint64_t, void*);
typedef float (*RR_GET_CAMERA_FOV)(uint64_t, void*);

static SRWLOCK g_camera_state_lock = SRWLOCK_INIT;
static RR_V3 g_camera_target;
static DWORD g_camera_target_at;
static uint64_t g_camera_target_model;
static uint64_t g_camera_target_id, g_camera_target_world;
static int g_camera_smooth;
static RR_CAMERA g_output_camera;
static DWORD g_output_camera_at;
static uint64_t g_output_camera_world;

static RR_Q4 camera_look(float yaw, float pitch) {
    const float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
    const float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    return (RR_Q4){cy*sp, sy*cp, -sy*sp, cy*cp};
}

static RR_Q4 camera_blend(RR_Q4 from, RR_Q4 to, int smooth, DWORD elapsed) {
    if (smooth <= 0) return to;
    const float norm = sqrtf(from.x*from.x + from.y*from.y + from.z*from.z + from.w*from.w);
    if (!(norm > 0.5f && norm < 1.5f)) return to;
    from = (RR_Q4){from.x/norm, from.y/norm, from.z/norm, from.w/norm};
    float dot = from.x*to.x + from.y*to.y + from.z*to.z + from.w*to.w;
    if (dot < 0.0f) {
        to = (RR_Q4){-to.x,-to.y,-to.z,-to.w};
        dot = -dot;
    }
    const float mix = 1.0f - expf(-(float)elapsed / (12.0f + 8.0f*smooth));
    float a = 1.0f - mix, b = mix;
    if (dot < 0.9995f) {
        const float angle = acosf(dot), inverse = 1.0f / sinf(angle);
        a = sinf(a*angle) * inverse;
        b = sinf(b*angle) * inverse;
    }
    RR_Q4 q = {a*from.x+b*to.x, a*from.y+b*to.y, a*from.z+b*to.z, a*from.w+b*to.w};
    const float length = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    return (RR_Q4){q.x/length,q.y/length,q.z/length,q.w/length};
}

static RR_V3 camera_rotate(RR_Q4 q, RR_V3 v) {
    const RR_V3 t = {2*(q.y*v.z-q.z*v.y), 2*(q.z*v.x-q.x*v.z), 2*(q.x*v.y-q.y*v.x)};
    return (RR_V3){v.x+q.w*t.x+q.y*t.z-q.z*t.y,
                   v.y+q.w*t.y+q.z*t.x-q.x*t.z,
                   v.z+q.w*t.z+q.x*t.y-q.y*t.x};
}

static int install_camera_push(void);
static void capture_muzzle(uint64_t world);
static void local_model_frame(int enabled);


static void camera_apply_look(RR_Q4 rotation, uint64_t world) {
    const uint64_t battle = g_battle, model = g_battle_model;
    if (!PL(battle) || rr_u64(battle + RR_OFF_BATTLE_WORLD) != world ||
        !u_is(model, "BattleModel") || rr_u64(model + RR_OFF_BATTLE_ENTITIES) != battle) return;
    const uint64_t input = rr_u64(model + RR_OFF_BATTLE_ROTATION);
    if (!u_is(input, "PlayerRotationInputModel")) return;

    const RR_V3 forward = camera_rotate(rotation, (RR_V3){0,0,1});
    const float yaw = atan2f(forward.x, forward.z);
    const float pitch = -atan2f(forward.y, hypotf(forward.x, forward.z));
    if (!isfinite(yaw) || !isfinite(pitch)) return;
    typedef void (*SET_ROTATION)(uint64_t, float, float, float, void*);
    typedef void (*SET_LOOK)(uint64_t, uint64_t, void*);
    UNITY_FN(RR_RVA_SET_PLAYER_ROTATION, SET_ROTATION)(input, yaw, pitch, 0.0f, NULL);
    const uint64_t battle_input = rr_u64(model + 0x50);
    if (u_is(battle_input, "BattleInputModel")) {
        const float angles[2] = {yaw, pitch};
        uint64_t packed;
        memcpy(&packed, angles, sizeof packed);  /* System.Numerics.Vector2 by value */
        UNITY_FN(0x4519FE0u, SET_LOOK)(battle_input, packed, NULL);
    }
}

static void camera_push(uint64_t brain, void* state, void* method) {
    RR_PUSH_CAMERA push = (RR_PUSH_CAMERA)(uintptr_t)(g_module + RR_RVA_BRAIN_PUSH_STATE);
    const uint64_t world = g_world, model = g_cam_obj;
    __try { local_model_frame(br_get()->tung_tung_model); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    const uint64_t active_brain = cl_brain();
    if (!state || !PL(world) || !PL(model) || (active_brain && active_brain != brain)) {
        push(brain, state, method);
        return;
    }
    unsigned char adjusted[0xE8];
    memcpy(adjusted, state, sizeof adjusted);
    float fov;
    RR_V3 pos, correction;
    RR_Q4 rotation;
    memcpy(&fov, adjusted, sizeof fov);
    memcpy(&pos, adjusted + 0x4C, sizeof pos);
    memcpy(&correction, adjusted + 0x78, sizeof correction);
    memcpy(&rotation, adjusted + 0x58, sizeof rotation);
    pos.x += correction.x; pos.y += correction.y; pos.z += correction.z;

    RR_V3 model_pos;
    const int pose_matches = rr_read(model + RR_OFF_CAM_POS, &model_pos, sizeof model_pos) &&
        hypotf(hypotf(pos.x-model_pos.x, pos.y-model_pos.y), pos.z-model_pos.z) < 8.0f;

    const DWORD now = GetTickCount();
    AcquireSRWLockShared(&g_camera_state_lock);
    const float requested_fov = g_cl_fov;
    const uint64_t target_id = g_camera_target_id;
    const RR_V3 target = g_camera_target;
    const int smooth = g_camera_smooth;
    const int locked = target_id && g_camera_target_model == model && g_camera_target_world == world &&
                       now - g_camera_target_at <= 100;
    ReleaseSRWLockShared(&g_camera_state_lock);

    unsigned blend_hint;
    memcpy(&blend_hint, adjusted + 0x94, sizeof blend_hint);
    int changed = 0, aimed = 0;
    if (requested_fov >= 10.0f && !(blend_hint & 0x40u)) {
        memcpy(adjusted, &requested_fov, sizeof requested_fov);
        changed = 1;
    }
    static uint64_t previous_brain, previous_world, previous_model, previous_target;
    static DWORD previous_at;
    static RR_Q4 previous_rotation;
    if (locked && pose_matches && !(blend_hint & 3u)) {
        const float dx = target.x-pos.x, dy = target.y-pos.y, dz = target.z-pos.z;
        const float horizontal = hypotf(dx, dz);
        if (isfinite(horizontal) && isfinite(dy) && horizontal > 0.01f) {
            const RR_Q4 goal = camera_look(atan2f(dx, dz), -atan2f(dy, horizontal));
            const DWORD elapsed = now - previous_at;
            const int continuous = target_id == previous_target && brain == previous_brain &&
                                   model == previous_model && world == previous_world && elapsed <= 100;
            rotation = camera_blend(continuous ? previous_rotation : rotation, goal, smooth,
                                    continuous ? elapsed : 8);
            const RR_Q4 rotation_correction = {0,0,0,1};
            memcpy(adjusted + 0x58, &rotation, sizeof rotation);
            memcpy(adjusted + 0x84, &rotation_correction, sizeof rotation_correction);
            const float dutch = 0.0f;
            memcpy(adjusted + 0x10, &dutch, sizeof dutch);
            changed = 1;
            aimed = 1;
        }
    }
    previous_target = aimed ? target_id : 0;
    previous_model = model;
    previous_world = world;
    previous_brain = brain;
    previous_at = now;
    previous_rotation = rotation;
    if (aimed) {
        __try { camera_apply_look(rotation, world); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    push(brain, changed ? adjusted : state, method);

    RR_OUTPUT_CAMERA output = (RR_OUTPUT_CAMERA)(uintptr_t)(g_module + RR_RVA_BRAIN_OUTPUT_CAMERA);
    RR_GET_CAMERA_FOV get_fov = (RR_GET_CAMERA_FOV)(uintptr_t)(g_module + RR_RVA_CAMERA_GET_FOV);
    const uint64_t camera = output(brain, NULL);
    if (!u_is(camera, "Camera") || !PL(rr_u64(camera + 0x10))) return;
    const BR_Config* cfg = br_get();
    __try { world_lighting_frame(camera, cfg); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (cfg->show_tracers) {
        __try { capture_muzzle(world); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    fov = get_fov(camera, NULL);
    if (!(fov >= 1.0f && fov < 179.0f) || model != g_cam_obj || world != g_world) return;

    RR_CAMERA rendered = {0};
    rendered.fov = fov;
    rendered.model_source = model;
    const uint64_t transform = UNITY_FN(0x627E130u, SC_GET_REF)(camera, NULL);
    if (scene_alive(transform) && u_is(transform, "Transform")) {
        UNITY_FN(0x62D0B80u, SC_GET_POS)(&rendered.pos, transform, NULL);
        UNITY_FN(0x62D0D80u, SC_GET_COLOR)(&rendered.rot, transform, NULL);
        const RR_Q4 q = rendered.rot;
        const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
        if (norm > 0.99f && norm < 1.01f &&
            isfinite(rendered.pos.x) && isfinite(rendered.pos.y) && isfinite(rendered.pos.z)) {
            rendered.right = camera_rotate(q, (RR_V3){1,0,0});
            rendered.up = camera_rotate(q, (RR_V3){0,1,0});
            rendered.fwd = camera_rotate(q, (RR_V3){0,0,1});
            rendered.valid = 1;
        }
    }
    AcquireSRWLockExclusive(&g_camera_state_lock);
    if (!rendered.valid && g_output_camera.model_source == model && g_output_camera_world == world) {
        ReleaseSRWLockExclusive(&g_camera_state_lock);
        return;
    }
    g_output_camera = rendered;
    g_output_camera_at = now;
    g_output_camera_world = world;
    ReleaseSRWLockExclusive(&g_camera_state_lock);
}

static int unity_scene_build(uint64_t* module) {
    uint32_t size;
    if (!rr_module("GameAssembly.dll", module, &size) || *module != g_module ||
        size != 0x08FDC000u) return 0;
    const uint32_t pe_header = (uint32_t)rr_i32(*module + 0x3C);
    return pe_header >= 0x40 && pe_header <= 0x1000 &&
        (uint32_t)rr_i32(*module + pe_header + 8) == 0x6AF58C69u;
}

/* Patch a verified call contained entirely inside a single aligned word. */
static int patch_scene_call(uint64_t word, unsigned offset, const unsigned char expected[8],
                            uintptr_t callback) {
    const uint64_t site = word+offset;
    unsigned char original[8];
    if (offset > 3 || !rr_read(word, original, sizeof original) ||
        memcmp(original, expected, sizeof original)) return 0;

    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uint64_t step = info.dwAllocationGranularity;
    const uint64_t aligned = site - site % step;
    unsigned char* relay = NULL;
    for (uint64_t distance = step; distance < 0x40000000 && !relay; distance += step) {
        const uint64_t hint = aligned + distance;
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery((void*)(uintptr_t)hint, &region, sizeof region) && region.State == MEM_FREE)
            relay = VirtualAlloc((void*)(uintptr_t)hint, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    if (!relay) return 0;
    unsigned char jump[14] = { 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    memcpy(jump + 6, &callback, sizeof callback);
    memcpy(relay, jump, sizeof jump);
    DWORD old;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, sizeof jump)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return 0;
    }


    const int32_t displacement = (int32_t)((int64_t)(uintptr_t)relay - (int64_t)(site + 5));
    memcpy(original + offset + 1, &displacement, sizeof displacement);
    LONG64 replacement, previous;
    memcpy(&replacement, original, sizeof replacement);
    memcpy(&previous, expected, sizeof previous);
    if (!VirtualProtect((void*)(uintptr_t)word, sizeof replacement, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return 0;
    }
    const LONG64 actual = InterlockedCompareExchange64((volatile LONG64*)(uintptr_t)word, replacement, previous);
    FlushInstructionCache(GetCurrentProcess(), (const void*)(uintptr_t)word, sizeof replacement);
    DWORD unused;
    VirtualProtect((void*)(uintptr_t)word, sizeof replacement, old, &unused);
    if (actual != previous) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return 0;
    }
    return 1;
}

static int install_camera_push(void) {
    static int installed;
    uint64_t module;
    if (installed) return 1;
    if (!unity_scene_build(&module)) return 0;
    static const unsigned char expected[] = {0xE8,0x83,0,0,0,0x48,0x8D,0x8F};
    installed = patch_scene_call(module+RR_RVA_BRAIN_PUSH_CALL,0,expected,(uintptr_t)camera_push);
    return installed;
}

int game_cam_track(const RR_CAMERA* cam, uint64_t target_id, RR_V3 target, int smooth) {
    if (!cam->valid || !cam->model_source || cam->model_source != g_cam_obj ||
        !isfinite(target.x) || !isfinite(target.y) || !isfinite(target.z) || !install_camera_push()) return 0;
    AcquireSRWLockExclusive(&g_camera_state_lock);
    g_camera_target = target;
    g_camera_target_id = target_id;
    g_camera_target_world = g_world;
    g_camera_smooth = smooth > 20 ? 20 : smooth;
    g_camera_target_model = cam->model_source;
    g_camera_target_at = GetTickCount();
    ReleaseSRWLockExclusive(&g_camera_state_lock);
    return 1;
}

void game_cam_clear(void) {
    AcquireSRWLockExclusive(&g_camera_state_lock);
    g_camera_target_model = 0;
    g_camera_target_id = 0;
    g_camera_target_at = 0;
    ReleaseSRWLockExclusive(&g_camera_state_lock);
}

void game_cam_render_state(RR_CAMERA* cam) {
    AcquireSRWLockShared(&g_camera_state_lock);
    if (g_output_camera.model_source && g_output_camera.model_source == cam->model_source &&
        g_output_camera_world == g_world && GetTickCount() - g_output_camera_at <= 150) {
        if (g_output_camera.valid) *cam = g_output_camera;
    }
    ReleaseSRWLockShared(&g_camera_state_lock);
}
#endif

static uint64_t cl_rot_model(void) {
    const uint64_t aim = g_aim_updater;
    if (!PL(aim) || !class_named(rr_u64(aim), "BattleAimTargetUpdater")) return 0;
    const uint64_t rot = rr_u64(aim + RR_OFF_AIM_ROT_INPUT);
    return PL(rot) && class_named(rr_u64(rot), "PlayerRotationInputModel") ? rot : 0;
}

/* The active virtual camera owns the lens the brain samples. Its pointer sits
 * on the brain as an IL2CPP interface reference, which is the object pointer. */
static uint64_t cl_vcam(void) {
    const uint64_t brain = cl_brain();
    if (!PL(brain)) return 0;
    const uint64_t vcam = rr_u64(brain + RR_OFF_BRAIN_ACTIVE_VCAM);
    float lens = 0.0f;
    if (!PL(vcam) || !rr_read(vcam + RR_OFF_VCAM_LENS, &lens, sizeof lens)) return 0;
    return (lens > 1.0f && lens < 180.0f) ? vcam : 0;
}

static void cl_apply_fov(uint64_t brain, uint64_t vcam, float degrees) {
    if (PL(brain)) rr_write(brain + RR_OFF_BRAIN_FOV, &degrees, sizeof degrees);
    if (PL(vcam)) rr_write(vcam + RR_OFF_VCAM_LENS, &degrees, sizeof degrees);
}

static int cl_cam_vec(uint32_t off, RR_V3* out) {
    const uint64_t cam = g_cam_obj;
    if (!PL(cam) || !rr_read(cam + off, out, sizeof *out)) return 0;
    return isfinite(out->x) && isfinite(out->y) && isfinite(out->z);
}

/* Engage: read the model's units and axis signs against the live camera once.
 * Everything after this is a straight error term in degrees. */
static int cl_engage(void) {
    const uint64_t rot = cl_rot_model();
    if (!rot) return 0;
    float yaw = 0.0f, pitch = 0.0f;
    RR_V3 fwd;
    if (!rr_read(rot + RR_OFF_ROTIN_YAW, &yaw, sizeof yaw) ||
        !rr_read(rot + RR_OFF_ROTIN_PITCH, &pitch, sizeof pitch) ||
        !isfinite(yaw) || !isfinite(pitch) || !cl_cam_vec(RR_OFF_CAM_FWD, &fwd)) return 0;
    const float cam_yaw = atan2f(fwd.x, fwd.z) * RR_CL_RAD2DEG;
    const float cam_pitch = asinf(fmaxf(-1.0f, fminf(1.0f, fwd.y))) * RR_CL_RAD2DEG;
    const float d_deg = fabsf(cl_wrap180(yaw - cam_yaw));
    const float d_rad = fabsf(cl_wrap180(yaw * RR_CL_RAD2DEG - cam_yaw));
    g_cl_yaw_unit = d_deg <= d_rad ? 1.0f : 1.0f / RR_CL_RAD2DEG;
    g_cl_pitch_unit = g_cl_yaw_unit;
    const float m_yaw = yaw * g_cl_yaw_unit, m_pitch = pitch * g_cl_pitch_unit;
    g_cl_yaw_sign = fabsf(cl_wrap180(m_yaw - cam_yaw)) <= fabsf(cl_wrap180(-m_yaw - cam_yaw))
                    ? 1.0f : -1.0f;
    g_cl_pitch_sign = fabsf(m_pitch - cam_pitch) <= fabsf(-m_pitch - cam_pitch) ? 1.0f : -1.0f;
    g_cl_pt_s_valid = 0;
    g_cl_rot = rot;
    return 1;
}

/* One batched read: camera eye, camera forward and the rotation pair. Field-
 * sized on purpose - a single 0x50-byte block read can run past the camera
 * object into the next page and die. */
static int cl_read_tick(uint64_t rot, RR_V3* eye, RR_V3* fwd, float* m_yaw, float* m_pitch) {
    float pair[2];
    const uint64_t cam = g_cam_obj;
    if (!PL(cam) || !PL(rot)) return 0;
    RR_RD_REQ req[3] = {
        { cam + RR_OFF_CAM_POS, (uint32_t)sizeof *eye, eye },
        { cam + RR_OFF_CAM_FWD, (uint32_t)sizeof *fwd, fwd },
        { rot + RR_OFF_ROTIN_YAW, (uint32_t)sizeof pair, pair },
    };
    if (rr_read_batch(req, 3) != 3) return 0;
    memcpy(m_yaw, &pair[0], sizeof *m_yaw);
    memcpy(m_pitch, &pair[1], sizeof *m_pitch);
    const float fl = fwd->x*fwd->x + fwd->y*fwd->y + fwd->z*fwd->z;
    return isfinite(eye->x) && isfinite(eye->y) && isfinite(eye->z) &&
           isfinite(fl) && fl > 0.81f && fl < 1.21f &&
           isfinite(*m_yaw) && isfinite(*m_pitch);
}

static DWORD WINAPI cam_lock_worker(LPVOID p) {
    (void)p;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const LONGLONG slot = freq.QuadPart / RR_CL_POLL_HZ;
    uint64_t rot = 0;
    unsigned tick = 0;
    for (;;) {
        const int on = InterlockedCompareExchange(&g_cl_on, 0, 0) != 0;
        const float fov = g_cl_fov;
        if (!on) {
            /* FOV alone does not need the fast window. */
            if (fov > 1.0f) { cl_apply_fov(cl_brain(), cl_vcam(), fov); Sleep(2); }
            else Sleep(10);
            continue;
        }
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        if (!InterlockedCompareExchange(&g_cl_engaged, 0, 0)) {
            if (!cl_engage()) { Sleep(2); continue; }
            InterlockedExchange(&g_cl_engaged, 1);
            tick = 0;
        }
        /* Respawns and scene swaps move the model; re-resolve at ~16 Hz. */
        if ((tick++ & 63u) == 0u) {
            rot = cl_rot_model();
            if (!rot || rot != g_cl_rot) { InterlockedExchange(&g_cl_engaged, 0); continue; }
        }
        RR_V3 eye, fwd;
        float m_yaw = 0.0f, m_pitch = 0.0f;
        const int live = rot && cl_read_tick(rot, &eye, &fwd, &m_yaw, &m_pitch);
        if (live) {
            const float dt = 1.0f / (float)RR_CL_POLL_HZ;
            const DWORD now = timeGetTime();
            const DWORD pt_at = g_cl_pt_at;
            /* A point nothing refreshed is a ghost: menu, focus loss or a
             * dropped target stop the feed and the lock must hold still. */
            if (pt_at && now - pt_at <= RR_CL_POINT_MAX_MS) {
                const float px = g_cl_pt[0], py = g_cl_pt[1], pz = g_cl_pt[2];
                if (g_cl_pt_s_valid) {
                    const float jx = px - g_cl_pt_s[0], jy = py - g_cl_pt_s[1], jz = pz - g_cl_pt_s[2];
                    if (jx*jx + jy*jy + jz*jz > 2.25f) g_cl_pt_s_valid = 0;
                }
                if (!g_cl_pt_s_valid) {
                    g_cl_pt_s[0] = px; g_cl_pt_s[1] = py; g_cl_pt_s[2] = pz;
                    g_cl_pt_s_valid = 1;
                } else {
                    const float blend = fminf(1.0f, dt / 0.015f);
                    g_cl_pt_s[0] += (px - g_cl_pt_s[0]) * blend;
                    g_cl_pt_s[1] += (py - g_cl_pt_s[1]) * blend;
                    g_cl_pt_s[2] += (pz - g_cl_pt_s[2]) * blend;
                }
                const float dx = g_cl_pt_s[0] - eye.x;
                const float dy = g_cl_pt_s[1] - eye.y;
                const float dz = g_cl_pt_s[2] - eye.z;
                const float rlen = sqrtf(dx*dx + dy*dy + dz*dz);
                if (rlen > 0.05f) {
                    const float cam_yaw = atan2f(fwd.x, fwd.z) * RR_CL_RAD2DEG;
                    const float cam_pitch = asinf(fmaxf(-1.0f, fminf(1.0f, fwd.y))) * RR_CL_RAD2DEG;
                    const float des_yaw = atan2f(dx, dz) * RR_CL_RAD2DEG;
                    const float des_pitch = asinf(fmaxf(-1.0f, fminf(1.0f, dy / rlen))) * RR_CL_RAD2DEG;
                    const float err_yaw = cl_wrap180(des_yaw - cam_yaw);
                    const float err_pitch = des_pitch - cam_pitch;
                    const float alpha = g_cl_smooth <= 0.5f ? 1.0f
                        : fminf(1.0f, dt / (RR_CL_TAU_S + 0.045f * g_cl_smooth));
                    const float step_yaw = err_yaw * alpha, step_pitch = err_pitch * alpha;
                    if (fabsf(step_yaw) > 0.01f || fabsf(step_pitch) > 0.01f) {
                        const float limit = 89.0f * g_cl_pitch_unit;
                        float yaw_out = m_yaw + g_cl_yaw_sign * step_yaw * g_cl_yaw_unit;
                        float pitch_out = m_pitch + g_cl_pitch_sign * step_pitch * g_cl_pitch_unit;
                        if (pitch_out > limit) pitch_out = limit;
                        else if (pitch_out < -limit) pitch_out = -limit;
                        if (isfinite(yaw_out) && isfinite(pitch_out)) {
                            rr_write(rot + RR_OFF_ROTIN_YAW, &yaw_out, sizeof yaw_out);
                            rr_write(rot + RR_OFF_ROTIN_PITCH, &pitch_out, sizeof pitch_out);
                        }
                    }
                }
            }
        }
        if (fov > 1.0f) cl_apply_fov(cl_brain(), cl_vcam(), fov);
        for (;;) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart - start.QuadPart >= slot) break;
            Sleep(0);
        }
    }
    return 0;
}

static void cam_lock_start(void) {
    if (InterlockedCompareExchange(&g_cl_thread, 1, 0) != 0) return;
    HANDLE th = rr_thread_start(cam_lock_worker, NULL);
    if (th) CloseHandle(th);
    else InterlockedExchange(&g_cl_thread, 0);
}

void game_cam_lock(int on, RR_V3 target, float smooth) {
    if (!on) {
        InterlockedExchange(&g_cl_on, 0);
        InterlockedExchange(&g_cl_engaged, 0);
        g_cl_pt_s_valid = 0;
        g_cl_pt_at = 0;
        return;
    }
    if (!isfinite(target.x) || !isfinite(target.y) || !isfinite(target.z)) return;
    g_cl_pt[0] = target.x;
    g_cl_pt[1] = target.y;
    g_cl_pt[2] = target.z;
    /* Publish the feed time last: the worker treats an old stamp as a dead
     * target and stops steering, so anything that stops calling this cannot
     * drag the view at a ghost. */
    g_cl_pt_at = timeGetTime();
    /* 0 means instant: no slew, no smoothing, absolute write per tick. */
    g_cl_smooth = (isfinite(smooth) && smooth > 0.0f) ? smooth : 0.0f;
    InterlockedExchange(&g_cl_on, 1);
    cam_lock_start();
}

void game_cam_fov(float degrees) {
#ifdef RR_INTERNAL
    const float requested = (isfinite(degrees) && degrees >= 10.0f) ? fminf(degrees, 160.0f) : 0.0f;
    static int attempted;
    const BR_Config* cfg = br_get();
    if ((requested || cfg->world_sun || cfg->world_sky_glow || cfg->world_fog_custom ||
         cfg->world_fog_off || cfg->world_sky_off || cfg->world_sky_color ||
         cfg->show_tracers || cfg->enabled || cfg->tung_tung_model) &&
        !attempted && g_module) {
        attempted = install_camera_push() ? 1 : -1;
        if (attempted < 0) rr_int_log("camera > current GameAssembly layout did not match the scene hooks");
    }
    AcquireSRWLockExclusive(&g_camera_state_lock);
    g_cl_fov = requested;
    ReleaseSRWLockExclusive(&g_camera_state_lock);
#else
    g_cl_fov = (isfinite(degrees) && degrees > 1.0f) ? degrees : 0.0f;
    if (g_cl_fov > 1.0f) cam_lock_start();
#endif
}


/* Init. */
/* Diagnostic-only bootstrap: identify the character class from the live
 * UnityEntry roster instead of doing a full class discovery scan. */
int game_pose_diag_init(uint64_t world_hint) {
    ENTRY_ROOTS roots;
    if (!entry_roots(&roots)) {
        memset(&roots,0,sizeof(roots));
        if (!u_is(world_hint,"WorldEntitiesModel")) return 0;
        roots.world=world_hint;
    }
    uint64_t entries=0; int n=0, stride=0;
    uint64_t dict=rr_u64(roots.world + RR_OFF_WEM_BATTLE_CHARS);
    if (!u_dict(dict,&n,&entries,&stride) || stride!=0x18 || n<1 || n>4096) return 0;
    for (int i=0;i<n;++i) {
        uint64_t obj=rr_u64(entries+(uint64_t)i*stride+0x10);
        if (u_is(obj,"BattleCharacterModel")) { g_k_char=u_klass(obj); break; }
    }
    if (!g_k_char) return 0;
    g_world=roots.world; g_k_world=u_klass(roots.world); g_battle=roots.battle;g_battle_model=roots.battle_model;
    g_views_updater=roots.views;
    g_cam_obj=roots.camera; g_k_cam=roots.camera?u_klass(roots.camera):0;
    if (!g_cs_ready) { InitializeCriticalSection(&g_cache_cs); g_cs_ready=1; }
    xf_pool_init();
    find_heap_regions();
    instance_scan(g_inst_lo,g_inst_hi);
    if (InterlockedCompareExchange(&g_worker_started,1,0)==0) {
        HANDLE worker=rr_thread_start(scan_worker,NULL);
        if (worker) CloseHandle(worker); else InterlockedExchange(&g_worker_started,0);
    }
    return 1;
}
int game_init(void) {
    uint32_t size = 0;
    game_phase_set(GAME_PHASE_INIT_MODULE);
    if (!rr_module("GameAssembly.dll", &g_module, &size)) {
        set_status("GameAssembly.dll not found");
        game_phase_set(GAME_PHASE_IDLE);
        return 0;
    }
    game_phase_set(GAME_PHASE_INIT_RUNTIME);
    runtime_load();
    chain_load("robrun_chain.txt");

    if (!g_cs_ready) { InitializeCriticalSection(&g_cache_cs); g_cs_ready = 1; }
    xf_pool_init();

    /* Dynamic discovery: nothing from the previous process is trusted.
     * The region list must exist before class discovery: collect_hits walks it
     * and finds nothing on an empty list. */
    game_phase_set(GAME_PHASE_INIT_CLASSES);
    find_heap_regions();
    if (!g_k_char || !class_named(g_k_char, "BattleCharacterModel"))
        g_k_char = 0;
    if (!g_k_char) g_k_char = resolve_class("BattleCharacterModel");
    if (!g_k_char) {
        /* Keep the retry worker alive even without the class: it resolves it
         * again on its own, so recovery never waits for a menu re-init. */
        set_status("BattleCharacterModel class not found [%s]", g_dbg);
        if (InterlockedCompareExchange(&g_worker_started, 1, 0) == 0) {
            if (!g_views_ready_event) g_views_ready_event = CreateEventW(NULL, FALSE, FALSE, NULL);
            HANDLE worker = rr_thread_start(scan_worker, NULL);
            if (worker) CloseHandle(worker);
            else InterlockedExchange(&g_worker_started, 0);
        }
        return 0;
    }
    if (!g_k_cam || !class_named(g_k_cam, "BattleCameraModel")) g_k_cam = 0;
    if (!g_k_cam) g_k_cam = resolve_camera_class("BattleCameraModel");

    /* The scene-level battle container is not used for roster reads. Avoid
     * scanning the entire process for it during every initialization. */
    game_phase_set(GAME_PHASE_INIT_ENTRY);
    ENTRY_ROOTS roots;
    if (entry_roots(&roots)) {
        g_world = roots.world;
        g_k_world = u_klass(roots.world);
        g_battle = roots.battle;
        g_battle_model = roots.battle_model;
        g_views_updater = roots.views;
        if (roots.camera) g_cam_obj = roots.camera;
    }
    if (g_k_world && !class_named(g_k_world, "WorldEntitiesModel")) g_k_world = 0;
    if (!g_k_world) g_k_world = resolve_world_class();

    /* A heap-wide sweep stalls the title for tens of seconds, which reads as a
     * freeze right after injection. With a live world the roster is cheap, so
     * only fall back to sweeping after the scene walk has failed repeatedly. */
    int n = 0;
    const LONG tries = InterlockedIncrement(&g_init_tries);
    if (PL(g_world) || PL(g_battle) || tries > 8) {
        if (g_k_world && !PL(g_world) && g_saved_pid == g_rr_pid &&
            PL(g_saved_world) && rr_u64(g_saved_world) == g_k_world)
            g_world = g_saved_world;
        if (!PL(g_world) && g_k_world) g_world = find_world_instance();
        game_phase_set(GAME_PHASE_INIT_SCAN);
        n = instance_scan(g_inst_lo, g_inst_hi);
        g_last_scan = GetTickCount();
    }
    if (n > 0 && (!g_cam_obj || !camera_validate(g_cam_obj))) {
        game_phase_set(GAME_PHASE_CAMERA_SCAN);
        g_cam_obj = find_camera_scan();
    }
    runtime_save();

    if (InterlockedCompareExchange(&g_worker_started, 1, 0) == 0) {
        if (!g_views_ready_event) g_views_ready_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE worker = rr_thread_start(scan_worker, NULL);
        if (worker) CloseHandle(worker);
        else InterlockedExchange(&g_worker_started, 0);
    }
    /* Most diagnostic fields first: the menu column clips the tail. */
    set_status("ros=%ld wf=%ld bf=%ld st=%ld hits=%ld scan=%d w=%llx k=%llx reg=%d cam=%s",
               (long)g_diag_roster, (long)g_diag_wf, (long)g_diag_bf, (long)g_diag_stage,
               (long)g_diag_hits, n, (unsigned long long)g_world,
               (unsigned long long)g_k_world, g_nregions,
               camera_near_local(g_cam_obj) == 1 ? "model" : "local");
    game_phase_set(GAME_PHASE_IDLE);
    return n > 0;
}

int game_discovery_active(void) { return InterlockedCompareExchange(&g_worker_started, 0, 0) != 0; }
unsigned game_world_generation(void) { return world_gen(); }

/* True once the scene worker holds a live world with at least one roster
 * entry. The init retry sleeps on this instead of a fixed interval, so a
 * join is picked up within half a second of the roster appearing. */
int game_roster_ready(void) {
    if (!PL(g_world) && !PL(g_battle)) return 0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    const int n = g_ncache;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    return n > 0;
}

#ifdef RR_PORT_DIAGNOSTICS
int game_is_aiming(void) {
    uint64_t local = 0, world = g_world;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    for (int i = 0; i < g_ncache; ++i) if (g_cache[i].local) { local = g_cache[i].obj; break; }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    if (!PL(world) || !PL(local) || rr_u64(world) != g_k_world || rr_u64(local) != g_k_char) return 0;
    uint64_t priv = rr_u64(local + 0x10);
    if (!PL(priv) || !class_named(rr_u64(priv), "BattleCharacterPrivateModel")) return 0;
    uint64_t aiming = rr_u64(priv + 0x1F0);
    if (!PL(aiming) || !class_named(rr_u64(aiming), "AimingModel")) return 0;
    uint64_t data = rr_u64(aiming + 0x10);
    if (!PL(data) || !class_named(rr_u64(data), "BoolData")) return 0;
    uint8_t value = 0;
    return rr_read(data + 0x18, &value, 1) && value == 1 && world == g_world;
}

int game_shot_event(void) {
    static RR_THREAD_LOCAL uint64_t cached_world, cached_local, cached_event;
    static RR_THREAD_LOCAL DWORD validated_at;
    uint64_t local=0, world=g_world;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    for (int i=0;i<g_ncache;++i) if (g_cache[i].local) { local=g_cache[i].obj; break; }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    if (!PL(world) || !PL(local)) { cached_event=0; return -1; }
    DWORD now=GetTickCount();
    if (!PL(cached_event) || cached_world!=world || cached_local!=local || now-validated_at>=100) {
        cached_event=0;
        if (rr_u64(world)!=g_k_world || rr_u64(local)!=g_k_char) return -1;
        uint64_t priv=rr_u64(local+0x10);
        if (!PL(priv) || !class_named(rr_u64(priv),"BattleCharacterPrivateModel")) return -1;
        uint64_t event=rr_u64(priv+0x18);
        if (!PL(event) || !class_named(rr_u64(event),"EventModel") || world!=g_world) return -1;
        cached_world=world; cached_local=local; cached_event=event; validated_at=now;
    }
    uint8_t invoked=0;
    if (!rr_read(cached_event+0x10,&invoked,1) || invoked>1 || world!=g_world) { cached_event=0; return -1; }
    return invoked ? 1 : 0;
}


#endif
/* This is an inference, not replicated hidden state. Keep it bounded and
 * world-scoped so a disconnect cannot create a permanent ghost. */
typedef struct {
    int used, id, near_object, seen;
    int object_id, voice_candidate;
    uint64_t identity;
    DWORD last_seen, last_near_check, last_near;
    RR_V3 object_pos;
    RR_V3 object_anchor;
    unsigned relationships;
    char name[RR_NAME_MAX], role[20], weapon[20];
} HIDE_GUESS;
static HIDE_GUESS g_hide_guesses[RR_MAX_PLAYERS];
static uint64_t g_guess_world;

static int update_hide_guesses(RR_PLAYER* out, int count, int max, uint64_t world,
                               const HIDE_OBJECT* objects, int nobjects, DWORD now) {
    if (!world || g_guess_world != world) {
        memset(g_hide_guesses, 0, sizeof(g_hide_guesses));
        g_guess_world = world;
    }
    if (!world) return count;
    int have_local = 0;
    for (int i = 0; i < count; ++i) if (out[i].valid && out[i].local == RR_LOCAL_CONFIRMED) have_local = 1;
    if (!have_local) return count;
    for (int j = 0; j < RR_MAX_PLAYERS; ++j) g_hide_guesses[j].seen = 0;
    for (int i = 0; i < count; ++i) {
        RR_PLAYER* p = &out[i];
        if (!p->valid || p->local || p->is_loot || p->is_vehicle || p->id <= 0) continue;
        HIDE_GUESS* slot = NULL; HIDE_GUESS* free_slot = NULL;
        for (int j = 0; j < RR_MAX_PLAYERS; ++j) {
            HIDE_GUESS* g = &g_hide_guesses[j];
            if (g->used && g->id == p->id) { slot = g; break; }
            if (!free_slot && (!g->used || now - g->last_seen > 60000)) free_slot = g;
        }
        if (!slot) { if (!free_slot) continue; slot = free_slot; memset(slot, 0, sizeof(*slot)); slot->used = 1; slot->id = p->id; }
        slot->seen = 1; slot->last_seen = now; slot->relationships = p->relationships;
        snprintf(slot->name, sizeof(slot->name), "%s", p->name);
        snprintf(slot->role, sizeof(slot->role), "%s", p->role);
        snprintf(slot->weapon, sizeof(slot->weapon), "%s", p->weapon);
        if (p->hidden_in_object || p->inveh) { slot->near_object = 0; continue; }
        if (!slot->last_near_check || now - slot->last_near_check >= 200) {
            slot->last_near_check = now; slot->near_object = 0; float closest = 4.0f;
            for (int j = 0; j < nobjects; ++j) {
                RR_V3 interaction=objects[j].has_interaction?objects[j].anchor:objects[j].pos;
                float dx = p->pos.x - interaction.x, dz = p->pos.z - interaction.z;
                float dy = fabsf(p->pos.y - objects[j].pos.y), d2 = dx*dx + dz*dz;
                if (dy <= 2.5f && d2 <= closest) {
                    closest = d2; slot->near_object = 1; slot->object_pos = objects[j].pos;
                    slot->object_id=objects[j].id;slot->object_anchor=objects[j].anchor;
                }
            }
            if (slot->near_object) slot->last_near = now;
        }
    }
    for (int j = 0; j < RR_MAX_PLAYERS; ++j) {
        HIDE_GUESS* g = &g_hide_guesses[j];
        if (!g->used || g->seen) continue;
        DWORD age = now - g->last_seen;
        if (age > 60000) { memset(g, 0, sizeof(*g)); continue; }
        if (age < 700 || !g->near_object || g->last_seen - g->last_near > 1000 || count >= max) continue;
        int still_present=0;
        for(int o=0;o<nobjects;++o)if(objects[o].id==g->object_id){still_present=1;break;}
        if(!still_present)continue;
        RR_PLAYER* p = &out[count++]; memset(p, 0, sizeof(*p));
        p->health = -1;
        p->valid = 1; p->id = g->id; p->relationships = g->relationships;
        p->hidden_in_object = 2; p->pos=g->object_pos;p->head=g->object_anchor;
        snprintf(p->name, sizeof(p->name), "%s", g->name);
        snprintf(p->role, sizeof(p->role), "%s", g->role);
        snprintf(p->weapon, sizeof(p->weapon), "%s", g->weapon);
    }
    return count;
}

/* Slot membership survives streamed-out character models. Account identity
 * prevents a recycled slot from inheriting the previous player's guess. */
static int apply_hide_signals(RR_PLAYER* out,int count,int max,uint64_t world,
                              const RR_HIDE_SLOT* slots,int nslots,int slots_known,
                              const HIDE_OBJECT* objects,int nobjects) {
    if(g_guess_world!=world){memset(g_hide_guesses,0,sizeof(g_hide_guesses));g_guess_world=world;}
    if(!slots_known)return count;
    int local_id=-1;
    for(int i=0;i<count;++i)if(out[i].valid&&out[i].local==RR_LOCAL_CONFIRMED)local_id=out[i].id;
    for(int j=0;j<RR_MAX_PLAYERS;++j) {
        HIDE_GUESS* g=&g_hide_guesses[j];if(!g->used)continue;
        const RR_HIDE_SLOT* slot=NULL;
        for(int s=0;s<nslots;++s)if(slots[s].id==g->id){slot=&slots[s];break;}
        if(!slot||(g->identity&&slot->identity&&g->identity!=slot->identity)) {memset(g,0,sizeof(*g));continue;}
        if(slot->identity)g->identity=slot->identity;
        /* A default -1 alone has not been validated as an absence signal.
           Only clear a voice candidate after observing its positive->-1. */
        if(g->voice_candidate&&slot->voice_known&&slot->voice_hide_id==-1) {
            g->near_object=0;g->voice_candidate=0;
        }
    }
    if(local_id<0)return count;
    for(int s=0;s<nslots;++s) {
        const RR_HIDE_SLOT* slot=&slots[s];
        if(slot->id==local_id||!slot->voice_known||slot->voice_hide_id<0)continue;
        const HIDE_OBJECT* object=NULL;
        for(int o=0;o<nobjects;++o)if(objects[o].id==slot->voice_hide_id){object=&objects[o];break;}
        if(!object)continue;
        int confirmed=object->occupant_known&&object->occupant==slot->id;
        if(object->occupant_known&&object->occupant>=0&&!confirmed)continue;
        RR_PLAYER* p=NULL;
        for(int i=0;i<count;++i)if(out[i].valid&&!out[i].is_vehicle&&!out[i].is_loot&&out[i].id==slot->id){p=&out[i];break;}
        HIDE_GUESS* history=NULL;
        for(int j=0;j<RR_MAX_PLAYERS;++j)if(g_hide_guesses[j].used&&g_hide_guesses[j].id==slot->id){history=&g_hide_guesses[j];break;}
        if(p&&(p->hidden_in_object==1||!confirmed)) {
            if(p->hidden_in_object==2&&history){history->voice_candidate=1;history->identity=slot->identity;}
            continue;
        }
        if(!p) {
            if(count>=max)continue;
            p=&out[count++];memset(p,0,sizeof(*p));p->id=slot->id;p->health=-1;p->armor=-1;p->crouch=-1;
            if(history) {
                p->relationships=history->relationships;
                snprintf(p->role,sizeof(p->role),"%s",history->role);
                snprintf(p->weapon,sizeof(p->weapon),"%s",history->weapon);
            }
            snprintf(p->name,sizeof(p->name),"%s",slot->name);
        }
        p->valid=1;p->hidden_in_object=confirmed?1:2;p->pos=object->pos;p->head=object->anchor;
        p->cham_valid=0;p->nbones=0;p->mesh_bones=0;p->full_rig=0;
        if(history){history->voice_candidate=1;history->identity=slot->identity;}
    }
    return count;
}

/* Party membership for the local player, refreshed about once a second.
 * Parties keeps entity id -> PartyModel in one dictionary, so the local entry
 * identifies the squad and every other key pointing at the same PartyModel is
 * a teammate. Both lookups are class-checked; a failed walk leaves the list
 * empty and the team check simply does not filter. */
#define RR_PARTY_MAX 16

typedef struct {
    uint64_t world;
    int32_t  local;
    DWORD    at;
    int      n;
    int32_t  ids[RR_PARTY_MAX];
} PARTY_CACHE;

static PARTY_CACHE g_party;

static void party_refresh(uint64_t world, int32_t local_id) {
    const DWORD now = GetTickCount();
    if (world == g_party.world && local_id == g_party.local &&
        now - g_party.at < 1000) return;
    g_party.world = world;
    g_party.local = local_id;
    g_party.at = now;
    g_party.n = 0;
    if (!PL(world) || !local_id) return;

    const uint64_t veh = rr_u64(world + RR_OFF_WORLD_VEHICLES);
    if (!u_is(veh, "VehiclesWorldModel")) return;
    const uint64_t parties = rr_u64(veh + RR_OFF_VWORLD_PARTIES);
    if (!u_is(parties, "Parties")) return;
    const uint64_t dict = rr_u64(parties + RR_OFF_PARTIES_BY_ENTITY);
    int n = 0, stride = 0;
    uint64_t entries = 0;
    if (!u_dict(dict, &n, &entries, &stride) || n > 4096 || stride != 0x18) return;

    uint64_t mine = 0;
    for (int i = 0; i < n && !mine; ++i) {
        uint8_t e[0x18];
        int32_t key;
        uint64_t pm;
        if (!rr_read(entries + (uint64_t)i * 0x18, e, sizeof e)) continue;
        memcpy(&key, e + 8, sizeof key);
        memcpy(&pm, e + 0x10, sizeof pm);
        if (key == local_id && u_is(pm, "PartyModel")) mine = pm;
    }
    if (!mine) return;
    for (int i = 0; i < n && g_party.n < RR_PARTY_MAX; ++i) {
        uint8_t e[0x18];
        int32_t key;
        uint64_t pm;
        if (!rr_read(entries + (uint64_t)i * 0x18, e, sizeof e)) continue;
        memcpy(&key, e + 8, sizeof key);
        memcpy(&pm, e + 0x10, sizeof pm);
        if (key != local_id && pm == mine) g_party.ids[g_party.n++] = key;
    }
}

static int party_has(int32_t id) {
    for (int i = 0; i < g_party.n; ++i)
        if (g_party.ids[i] == id) return 1;
    return 0;
}

static int snapshot_read(RR_PLAYER* out, int max, RR_CAMERA* cam) {
    if (!out || max <= 0) return 0;
    BR_Config* cfg = br_get();

    /* Published snapshot: copied under the lock so the scan worker can swap in
     * a new roster without the read thread ever seeing a half-written entry. */
    static ENT_CACHE snap[RR_MAX_PLAYERS];
    static VEH_CACHE vsnap[RR_MAX_VEHICLES];
    static LOOT_CACHE lsnap[2 * RR_MAX_LOOT + RR_MAX_WORLD_MARKERS];
    static HIDE_OBJECT hide_snap[RR_MAX_HIDE_OBJECTS];
    static RR_HIDE_SLOT hide_slots[RR_MAX_PLAYERS];
    int nl = 0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    int n = g_ncache < max ? g_ncache : max;
    if (n > 0) memcpy(snap, g_cache, sizeof(ENT_CACHE) * (size_t)n);
    /* The roster is published by the scan worker, which is also the thread that
     * notices a world swap. Between those two moments the cached rows still
     * point into the unloaded scene, so the generation - not the class pointer -
     * decides whether a row may be read at all. */
    const unsigned roster_gen = world_gen();
    int fresh = 0;
    for (int i = 0; i < n; ++i) {
        if (snap[i].gen != roster_gen) continue;
        if (fresh != i) snap[fresh] = snap[i];
        ++fresh;
    }
    n = fresh;
    uint64_t vehicle_world = g_world;
    unsigned vehicle_generation = g_vehicle_generation;
    int want_vehicles = cfg->show_vehicle_markers;
#ifndef RR_INTERNAL
    want_vehicles |= cfg->vehicle_chams;
#endif
#ifdef RR_PORT_DIAGNOSTICS
    want_vehicles |= cfg->show_vehicles;
#endif
    int nv = want_vehicles && g_vehicle_world == vehicle_world ? g_nvehicles : 0;
    if (nv > 0) memcpy(vsnap, g_vehicles, sizeof(VEH_CACHE) * (size_t)nv);
    uint64_t loot_world = g_world;
    DWORD now = GetTickCount();
    int hide_fresh=g_hide_world==g_world&&now-g_hide_stamp<1500;
    int nhide = hide_fresh ? g_nhide_objects : 0;
    if (nhide > 0) memcpy(hide_snap, g_hide_objects, sizeof(hide_snap[0]) * (size_t)nhide);
    int hide_slots_known=hide_fresh&&g_hide_snapshot.slots_known;
    int nhide_slots=hide_slots_known?g_hide_snapshot.nslots:0;
    if(nhide_slots>0)memcpy(hide_slots,g_hide_snapshot.slots,sizeof(hide_slots[0])*(size_t)nhide_slots);
    const int loot_enabled[] = { cfg->show_loot, cfg->show_loot_nodes };
    for (int slot = 0; slot < 2; ++slot) {
        if (!loot_enabled[slot] || g_loot_world[slot] != loot_world || now - g_loot_stamp[slot] >= 5000) continue;
        int count = g_nloot[slot];
        memcpy(lsnap + nl, g_loot[slot], sizeof(LOOT_CACHE) * (size_t)count);
        nl += count;
    }
    if (g_markers_world == loot_world && now - g_markers_stamp < 5000) {
        memcpy(lsnap + nl, g_world_markers, sizeof(LOOT_CACHE) * (size_t)g_nworld_markers);
        nl += g_nworld_markers;
    }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);

    RR_CAMERA camv;
    memset(&camv, 0, sizeof(camv));

    DWORD tnow = GetTickCount();
    DWORD pose_now = timeGetTime();

    const ENT_CACHE* local = NULL;
    for (int i = 0; i < n; ++i) {
        if (snap[i].local) { local = &snap[i]; break; }
    }
    int local_is_police = local && strcmp(local->role, "cop") == 0;
    const char* local_clan = local ? local->clan_tag : "";
    party_refresh(vehicle_world, local ? local->id : 0);

    int written = 0;
    for (int i = 0; i < n && written < max; i++) {
        float f[6] = {0};
        if (snap[i].hidden_in_object) {
            f[0] = snap[i].hidden_pos.x; f[1] = snap[i].hidden_pos.y; f[2] = snap[i].hidden_pos.z;
        } else if (!rr_read(snap[i].tr + RR_OFF_CTM_X, f, TR_READ_LEN)) continue;
        if (!isfinite(f[0]) || !isfinite(f[1]) || !isfinite(f[2])) continue;
        if (fabsf(f[0]) > 500000.0f || fabsf(f[1]) > 500000.0f || fabsf(f[2]) > 500000.0f) continue;

        /* velocity from the previous sample, smoothed; resets on teleports */
        MOTION* m = motion_slot(snap[i].obj);
        m->t = tnow;
        m->ok = 1;
        RR_V3 sampled_pos = { f[0], f[1], f[2] };

        RR_PLAYER* p = &out[written];
        memset(p, 0, sizeof(*p));
        p->addr = snap[i].obj;
        p->id = snap[i].id;
        if (snap[i].cham_ready) p->cham_rig = snap[i].cham_native[0];
        p->health = -1;
#ifndef RR_INTERNAL
        if (cfg->show_health) {
            uint64_t data = rr_u64(snap[i].obj + RR_OFF_CHAR_DATA);
            uint64_t private_data = PL(data) ? rr_u64(data + RR_OFF_DATA_PRIVATE) : 0;
            if (u_is(private_data, "BattleCharacterPrivateData")) {
                uint64_t health = rr_u64(private_data + RR_OFF_PRIVATE_HEALTH); int32_t value = -1;
                if (u_is(health, "IntData") && rr_read(health + RR_OFF_PRIM_VALUE, &value, sizeof value) &&
                    value >= 0 && value <= 10000) {
                    p->health = value;
                    /* Il2CppDumper's BattleCharacterPrivateModel.Health field.
                     * Only use its maximum if the model owns this exact IntData. */
                    const unsigned private_health_model = 0x1B0u;
                    uint64_t private_model = rr_u64(snap[i].obj + RR_OFF_CHAR_PRIVATE);
                    uint64_t health_model = u_is(private_model, "BattleCharacterPrivateModel")
                        ? rr_u64(private_model + private_health_model) : 0;
                    int32_t maximum = 0;
                    if (u_is(health_model, "HealthModel") &&
                        rr_u64(health_model + RR_OFF_HEALTH_VALUE) == health &&
                        rr_read(health_model + RR_OFF_HEALTH_MAX, &maximum, sizeof maximum) &&
                        maximum > 0 && maximum <= 10000) p->max_health = maximum;
                }
            }
        }
#endif
        p->armor = snap[i].armor;
        p->crouch = snap[i].crouch;
        p->local = snap[i].local;
        p->hidden_in_object = snap[i].hidden_in_object;
        if (!p->local) {
            if (local_clan[0] && strcmp(local_clan, snap[i].clan_tag) == 0)
                p->relationships |= RR_REL_CLAN;
            if (local_is_police && strcmp(snap[i].role, "cop") == 0)
                p->relationships |= RR_REL_POLICE;
            if (party_has(snap[i].id))
                p->relationships |= RR_REL_PARTY;
        }
        p->inveh = snap[i].inveh;
        p->vehicle_id = snap[i].vehicle_id;
        snprintf(p->veh, sizeof(p->veh), "%s", snap[i].veh);
        snprintf(p->role, sizeof(p->role), "%s", snap[i].role);
        snprintf(p->weapon, sizeof(p->weapon), "%s", snap[i].weapon);
        snprintf(p->name, sizeof(p->name), "%s", snap[i].name);
        p->pos.x = f[0]; p->pos.y = f[1]; p->pos.z = f[2];
        p->rot_yaw = f[4];
        p->head = p->pos;
        p->head.y += (snap[i].crouch == 1) ? 1.05f : 1.75f;
        if(p->hidden_in_object)p->head=snap[i].hidden_anchor;

        const int want_pose = needs_mesh_pose(cfg, p->inveh);
        int want_fallback_bones = cfg->show_box || cfg->show_skeleton || cfg->aa_on;
        int want_seated_mesh = cfg->show_box || cfg->cham_on || cfg->aa_on;
#ifdef RR_INTERNAL
        want_fallback_bones |= p->inveh && cfg->enabled;
        want_seated_mesh |= cfg->enabled;
#endif
        int mesh_ok=0;
        if (!p->hidden_in_object && (cfg->enabled || cfg->aa_on || (p->local && cfg->show_tracers)) && snap[i].mesh_h &&
            (!p->inveh || want_seated_mesh)) {
            /* A malformed or recycled rig must not suppress every ESP marker. */
            if (g_pose_diag_enabled && !p->local) ++g_pose_diag.attempts;
            __try { mesh_ok=build_mesh_bones(&snap[i],p,pose_now); }
            __except (EXCEPTION_EXECUTE_HANDLER) { mesh_ok=0; p->cham_valid=0; }
            if (g_pose_diag_enabled && !p->local) { if (mesh_ok) ++g_pose_diag.success; else ++g_pose_diag.failed; }
        }
        else if (g_pose_diag_enabled && !p->local && !p->hidden_in_object && !snap[i].mesh_h) ++g_pose_diag.missing_rig;
        if ((!mesh_ok || !p->full_rig) && !p->hidden_in_object &&
#ifndef RR_INTERNAL
            !p->local &&
#endif
            want_pose) {
            if (reuse_mesh_pose(&snap[i], m, p, sampled_pos, pose_now)) {
                mesh_ok = 1;
                if (g_pose_diag_enabled) ++g_pose_diag.reused_mesh;
            }
        }
        if (mesh_ok && !p->full_rig) {
            /* Anchor markers to the resolved rig for standing and seated players.
             * This keeps the head marker aligned with the displayed body instead of a
             * fixed-height estimate. build_mesh_bones() validates the pose before use.
             * The head slot is the crown, not the head joint: M_head_1 sits at eye
             * height, so the palette anchor is half the step toward M_head_2. Using
             * the eyelid index here put the box top across the eyes. */
            p->pos.x=p->bone[0][0]; p->pos.y=p->bone[0][1]; p->pos.z=p->bone[0][2];
            const float* h1 = p->bone[RR_RIG_M_HEAD_1_JNT];
            p->head.x=h1[0]; p->head.y=h1[1]; p->head.z=h1[2];
            const float* h2 = p->bone[RR_RIG_M_HEAD_2_JNT];
            if (p->nbones > RR_RIG_M_HEAD_2_JNT && isfinite(h2[0]) && isfinite(h2[1]) && isfinite(h2[2])) {
                const float dx = h2[0]-h1[0], dy = h2[1]-h1[1], dz = h2[2]-h1[2];
                /* A crown that far off the head joint is a bad read, not a tall hat. */
                if (dx*dx + dy*dy + dz*dz < 0.09f) {
                    p->head.x += 0.5f*dx; p->head.y += 0.5f*dy; p->head.z += 0.5f*dz;
                }
            }
        }
        if (!mesh_ok) {
            p->nbones = p->mesh_bones = p->full_rig = 0;
            if (!p->hidden_in_object && want_fallback_bones && snap[i].skel) {
                const int nb = build_bones(snap[i].skel, f[4], p->pos.x, p->pos.y, p->pos.z, p->bone);
                p->nbones = (nb > 0 && nb <= RR_MAX_BONES) ? nb : 0;
                if (p->nbones) memcpy(p->bone_group, g_skelcfg.group, sizeof(p->bone_group));
            }
        }
        /* Retain the last rendered pose through a brief read gap. The skeleton
         * and the box read this pose, so the hold has to follow the ESP
         * toggles, not just chams: a skeleton-only setup dropped the smoothed
         * pose every time a mesh read hiccuped. */
        if (!p->hidden_in_object && want_pose &&
            !p->local && !p->cham_valid) {
            reuse_cham_pose(&snap[i], m, p, sampled_pos, pose_now);
            if (g_pose_diag_enabled && p->cham_valid) ++g_pose_diag.reused_cham;
        }
        p->valid = 1;
        written++;
    }

    written=apply_hide_signals(out,written,max,vehicle_world,hide_slots,nhide_slots,hide_slots_known,hide_snap,nhide);
    written = update_hide_guesses(out, written, max, vehicle_world, hide_snap, nhide, tnow);
    /* New inference slots are created by update_hide_guesses; associate their
       current account before they can survive a disconnect or slot reuse. */
    written=apply_hide_signals(out,written,max,vehicle_world,hide_slots,nhide_slots,hide_slots_known,hide_snap,nhide);
    for(int i=0;i<written;++i)if(out[i].hidden_in_object) {
        float dx=out[i].pos.x-camv.pos.x,dy=out[i].pos.y-camv.pos.y,dz=out[i].pos.z-camv.pos.z;
        out[i].dist=sqrtf(dx*dx+dy*dy+dz*dz);
    }

    const VEH_CHAM_POSE* poses = cfg->vehicle_chams && nv > 0 && vehicle_world == g_world
        ? sample_vehicle_poses(vsnap, nv, vehicle_generation, GetTickCount()) : NULL;
    for (int i = 0; i < nv && written < max && vehicle_world == g_world; ++i) {
        const VEH_CACHE* vehicle = &vsnap[i];
        RR_V3 pos;
        int cham_ok = poses && poses[i].valid;
        if (cham_ok) pos = (RR_V3){poses[i].matrix[3], poses[i].matrix[7], poses[i].matrix[11]};
        int marker_ok = 0;
#ifdef RR_PORT_DIAGNOSTICS
        if (!cham_ok && cfg->show_vehicles) {
            uint8_t head[0x18]; uint64_t klass; int32_t id;
            if (!rr_read(vehicle->obj, head, sizeof(head))) continue;
            memcpy(&klass, head, 8); memcpy(&id, head + 0x10, 4);
            if (klass != vehicle->klass || id != vehicle->id) continue;
            if (!rr_read(vehicle->tr + 0x38, &pos, sizeof(pos)) ||
                !(fabsf(pos.x) <= 500000 && fabsf(pos.y) <= 500000 && fabsf(pos.z) <= 500000)) continue;
            RR_V3 rendered;
            marker_ok = vehicle_marker_position(vehicle, &pos, &rendered);
            if (marker_ok) pos = rendered;
        } else
#endif
        if (!cham_ok && cfg->show_vehicle_markers) {
            uint8_t head[0x18]; uint64_t klass; int32_t id;
            if (!rr_read(vehicle->obj, head, sizeof head)) continue;
            memcpy(&klass, head, 8); memcpy(&id, head + 0x10, 4);
            if (klass != vehicle->klass || id != vehicle->id ||
                !rr_read(vehicle->tr + 0x38, &pos, sizeof pos) ||
                !isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z) ||
                fabsf(pos.x) > 500000 || fabsf(pos.y) > 500000 || fabsf(pos.z) > 500000) continue;
        } else if (!cham_ok) continue;
        RR_PLAYER* p = &out[written++];
        memset(p, 0, sizeof(*p));
        p->valid = 1; p->is_vehicle = vehicle->kind + 1; p->id = vehicle->id; p->addr = vehicle->obj;
        p->pos = p->head = pos;
        p->occupants=vehicle->occupants; p->occupants_known=vehicle->occupants_known; p->vehicle_rendered=marker_ok;
        p->vehicle_cham_valid = cham_ok;
        if (cham_ok) memcpy(p->vehicle_pose, poses[i].matrix, sizeof(p->vehicle_pose));
        snprintf(p->name, sizeof(p->name), "%s", vehicle->name);
        snprintf(p->role, sizeof(p->role), "%s", vehicle->role);
    }

#ifdef RR_PORT_DIAGNOSTICS
    /* Occupancy is partial: only characters present in the current roster can
     * be counted. An absent count does not imply the vehicle is empty. */
    for (int i = 0; i < written; ++i) if (out[i].is_vehicle && !out[i].occupants_known)
        for (int j = 0; j < written; ++j)
            if (!out[j].is_vehicle && !out[j].is_loot && out[j].inveh && out[j].vehicle_id == out[i].id)
                ++out[i].occupants;

#endif
    if (loot_world == g_world) for (int i = 0; i < nl && written < max; ++i) {
        const LOOT_CACHE* loot = &lsnap[i];
        RR_PLAYER* p = &out[written++];
        memset(p, 0, sizeof(*p));
        p->valid = 1;
        p->is_loot = loot->kind;
        p->loot_rarity = loot->rarity;
        p->id = loot->id;
        p->pos = p->head = loot->pos;
        snprintf(p->name, sizeof(p->name), "%s", loot->name);
    }
    /* Sample the camera after entity reads so turning the view does not wait
     * behind the whole player/skeleton update. */
    int cam_ok = read_camera(&camv);
    if (cam) *cam = camv;
    if (cam_ok) for (int i = 0; i < written; ++i) {
        float dx = out[i].pos.x - camv.pos.x, dy = out[i].pos.y - camv.pos.y, dz = out[i].pos.z - camv.pos.z;
        out[i].dist = sqrtf(dx * dx + dy * dy + dz * dz);
    }

    int have_local = 0;
    for (int i = 0; i < written; i++) if (out[i].local) have_local = 1;
    if (!have_local) {
        int best = -1; float best_d = 1e30f;
        for (int i = 0; i < written; i++)
            if (!out[i].is_vehicle && !out[i].is_loot && out[i].dist > 0.0f && out[i].dist < best_d) { best_d = out[i].dist; best = i; }
        if (best >= 0 && best_d < 10.0f) out[best].local = RR_LOCAL_ESTIMATED;
    }
    return written;
}

/* Shot tracers.
 *
 * The local character keeps its current ray in ShotArgsModel (0xF8) and the
 * resolved contacts in ShotResultModel (0x100). Results land a few ticks after
 * the shot, so a new TargetTickIndex is what ports one batch: the args origin
 * plus every ContactPosition in the result array. A batch with no contact is a
 * miss and falls back to a fixed ray along the stored direction.
 *
 * ShotResult is a value type (InstanceSize 0x54, header 0x10), so array stride
 * is 0x44 and ContactPosition sits at +0x18 unboxed. Capture runs with
 * the snapshot sampler; the render thread copies completed segments.
 */
#define RR_OFF_CHAR_SHOTARGS  0xF8u   /* BattleCharacterModel.ShotArgsModel    */
#define RR_OFF_CHAR_SHOTRES   0x100u  /* BattleCharacterModel.ShotResultModel  */
#define RR_OFF_ARGS_POS       0x10u   /* IPrimitiveData<Vector3> _position     */
#define RR_OFF_ARGS_DIR       0x18u   /* IPrimitiveData<Vector3> _direction    */
#define RR_OFF_RES_ARRAY      0x10u   /* ShotResult[]                          */
#define RR_OFF_RES_HAS        0x18u   /* bool HasResult                        */
#define RR_OFF_RES_TICK       0x20u   /* long TargetTickIndex                  */
#define RR_OFF_RES_COUNT      0x28u   /* int Count                             */
#define RR_SHOTRES_STRIDE     0x44u
#define RR_SHOTRES_HAS        0x00u
#define RR_SHOTRES_CONTACT    0x18u   /* Vector3 ContactPosition               */
#define RR_SHOTRES_DIRECTION  0x30u   /* Vector3 ShotDirection                 */
#define RR_SHOT_SEGMENTS      8       /* widest result set the client draws    */
#define RR_SHOT_FALLBACK_M    150.0f
#define RR_TRACER_CAP         128

static struct { uint64_t local, world; int64_t tick; int initialized; } g_tr_seen;
static SRWLOCK g_tr_lock = SRWLOCK_INIT;
static RR_TRACER g_tr_ring[RR_TRACER_CAP];
static DWORD     g_tr_born[RR_TRACER_CAP];
static int       g_tr_head;

/* NaN fails every comparison and inf fails the range, so one test covers both. */
static int tr_plausible(const RR_V3* v) {
    return fabsf(v->x) < 1e6f && fabsf(v->y) < 1e6f && fabsf(v->z) < 1e6f;
}

/* Vector3Data is 0x10 bytes wider than IntData/BoolData in this dump while the
 * generic base hides its instance layout, so both candidate value slots are
 * probed. The look direction is unit length, which tells the right slot apart;
 * the position only has to be finite and non-zero. */
#ifndef RR_INTERNAL
static int tr_read_vec(uint64_t prim, RR_V3* out, int unit) {
    static const uint32_t kOffset[] = { RR_OFF_PRIM_VALUE, 0x20u };
    for (int i = 0; i < 2; ++i) {
        RR_V3 v;
        if (!rr_read(prim + kOffset[i], &v, sizeof v) || !tr_plausible(&v)) continue;
        if (unit) {
            const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
            if (len2 < 0.25f || len2 > 2.25f) continue;
        } else if (v.x == 0.0f && v.y == 0.0f && v.z == 0.0f) {
            continue;
        }
        *out = v;
        return 1;
    }
    return 0;
}
#else
static int tr_read_args(uint64_t args, RR_V3* origin, RR_V3* direction) {
    const unsigned position_rva = 0x4890320u, direction_rva = 0x4890250u;
    const uint64_t module = g_module;
    if (!module) return 0;
    const unsigned char expected[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x30,0x80,0x3D};
    unsigned char actual[sizeof(expected)];
    if (!rr_read(module + position_rva, actual, sizeof actual) || memcmp(actual, expected, sizeof actual) ||
        !rr_read(module + direction_rva, actual, sizeof actual) || memcmp(actual, expected, sizeof actual)) return 0;
    typedef RR_V3* (*GET_VECTOR)(RR_V3*, uint64_t, void*);
    __try {
        ((GET_VECTOR)(uintptr_t)(module + position_rva))(origin, args, NULL);
        ((GET_VECTOR)(uintptr_t)(module + direction_rva))(direction, args, NULL);
        return tr_plausible(origin) && tr_plausible(direction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
#endif

#ifdef RR_INTERNAL
static SRWLOCK g_muzzle_lock = SRWLOCK_INIT;
static struct { uint64_t world, local; DWORD at; RR_V3 point; } g_muzzle;
static struct { uint64_t world, local; DWORD at; RR_V3 point; } g_rig_muzzle;

static int tr_weapon_height(const RR_V3* point, const RR_V3* feet) {
    const float dx = point->x-feet->x, dz = point->z-feet->z;
    const float height = point->y-feet->y;
    return tr_plausible(point) && height > .34f && height < 2.7f && dx*dx + dz*dz < 6.25f;
}

static void tracer_rig_muzzle(const RR_PLAYER* players, int count, uint64_t world) {
    g_rig_muzzle.at = 0;
    for (int i=0; i<count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (p->local != RR_LOCAL_CONFIRMED || !p->full_rig || !p->valid) continue;
        const int joints[] = {RR_RIG_WEAPONE1_JNT, RR_RIG_R_ARM_3_JNT};
        for (int j=0; j<_countof(joints); ++j) {
            const float* bone = p->bone[joints[j]];
            const RR_V3 point = {bone[0], bone[1], bone[2]};
            if (!tr_weapon_height(&point, &p->pos)) continue;
            g_rig_muzzle.world = world;
            g_rig_muzzle.local = p->addr;
            g_rig_muzzle.point = point;
            g_rig_muzzle.at = GetTickCount();
            return;
        }
        return;
    }
}


static uint64_t local_character_view(uint64_t local) {
    const uint64_t updater = g_views_updater, battle = g_battle;
    if (!views_updater_valid(updater, battle) || rr_u64(battle + RR_OFF_BATTLE_WORLD) != g_world) return 0;
    uint64_t entries = 0;
    int n = 0, stride = 0;
    if (!u_dict(rr_u64(updater + RR_OFF_VIEWS_CHARACTERS), &n, &entries, &stride) ||
        n > 4096 || stride != 0x18) return 0;

    const int id = rr_i32(local + RR_OFF_CHAR_ID);
    for (int i=0; i<n; ++i) {
        const uint64_t entry = entries + (uint64_t)i*0x18;
        if (rr_i32(entry + 8) != id) continue;
        const uint64_t view = rr_u64(entry + 0x10);
        if (!u_is(view, "BattleCharacterEntityView")) continue;
        return view;
    }
    return 0;
}

#include "rig/rr_local_model.inc"

static void capture_muzzle(uint64_t world) {
    typedef uint64_t (*GET_CHILD)(uint64_t, uint64_t, unsigned char, void*);
    static uint64_t last_local, view, weapon_type;
    static DWORD view_at;
    const uint64_t local = sa_local();
    if (!local) return;
    const DWORD now = GetTickCount();
    if (local != last_local || !scene_alive(view) || now-view_at >= 250) {
        view = local_character_view(local);
        view_at = now;
        last_local = local;
    }
    if (!scene_alive(view)) return;
    if (!weapon_type) weapon_type = scene_type("CharacterWeaponView");
    if (!weapon_type) return;
    const uint64_t position = rr_u64(local + RR_OFF_CHAR_TRANSFORM);
    RR_V3 feet;
    if (!rr_read(position + RR_OFF_CTM_X, &feet, sizeof feet)) return;
    const unsigned roots[] = {0x48, 0x88, 0x90};
    for (int i=0; i<_countof(roots); ++i) {
        const uint64_t root = rr_u64(view + roots[i]);
        if (!u_is(root, "Transform") || !scene_alive(root)) continue;
        const uint64_t weapon = UNITY_FN(0x627D6B0u, GET_CHILD)(root, weapon_type, 0, NULL);
        const uint64_t muzzle = u_is(weapon, "CharacterWeaponView") ? rr_u64(weapon + 0x30) : 0;
        if (!u_is(muzzle, "Transform") || !scene_alive(muzzle)) continue;
        RR_V3 point;
        UNITY_FN(0x62D0B80u, SC_GET_POS)(&point, muzzle, NULL);
        if (!tr_weapon_height(&point, &feet)) continue;
        AcquireSRWLockExclusive(&g_muzzle_lock);
        g_muzzle.world = world;
        g_muzzle.local = local;
        g_muzzle.at = now;
        g_muzzle.point = point;
        ReleaseSRWLockExclusive(&g_muzzle_lock);
        return;
    }
}
#endif

static void tracer_push(const RR_V3* a, const RR_V3* b, int hit) {
    g_tr_ring[g_tr_head].a = *a;
    g_tr_ring[g_tr_head].b = *b;
    g_tr_ring[g_tr_head].hit = hit;
    g_tr_ring[g_tr_head].age_ms = 0;
    g_tr_born[g_tr_head] = timeGetTime();
    g_tr_head = (g_tr_head + 1) % RR_TRACER_CAP;
}

void game_update_tracers(int enabled) {
    static int was_enabled;
    if (!enabled && !was_enabled) return;
    was_enabled = enabled;
    const uint64_t local = sa_local();
    const uint64_t world = g_world;
    AcquireSRWLockExclusive(&g_tr_lock);
    if (!enabled || local != g_tr_seen.local || world != g_tr_seen.world) {
        memset(g_tr_born, 0, sizeof g_tr_born);
        g_tr_head = 0;
        g_tr_seen.local = local;
        g_tr_seen.world = world;
        g_tr_seen.initialized = 0;
    }
    const uint64_t res = enabled && local ? rr_u64(local + RR_OFF_CHAR_SHOTRES) : 0;
    if (!u_is(res, "ShotResultModel")) goto done;
    unsigned char has = 0;
    int64_t tick = 0;
    if (!rr_read(res + RR_OFF_RES_HAS, &has, 1) ||
        !rr_read(res + RR_OFF_RES_TICK, &tick, sizeof tick)) goto done;
    if (!g_tr_seen.initialized) {
        g_tr_seen.initialized = 1;
        g_tr_seen.tick = has ? tick : -1;
        goto done;
    }
    if (!has || tick == g_tr_seen.tick) goto done;

    const uint64_t args = rr_u64(local + RR_OFF_CHAR_SHOTARGS);
    if (!u_is(args, "ShotArgsModel")) goto done;
    RR_V3 origin, aim;
#ifdef RR_INTERNAL
    if (!tr_read_args(args, &origin, &aim)) goto done;
#else
    const uint64_t pos = rr_u64(args + RR_OFF_ARGS_POS);
    const uint64_t dir = rr_u64(args + RR_OFF_ARGS_DIR);
    if (!PL(pos) || !tr_read_vec(pos, &origin, 0)) goto done;
    if (!PL(dir) || !tr_read_vec(dir, &aim, 1)) goto done;
#endif

    const uint64_t arr = rr_u64(res + RR_OFF_RES_ARRAY);
    uint64_t data = 0;
    int len = PL(arr) ? u_array(arr, &data) : 0;
    int count = 0;
    if (!rr_read(res + RR_OFF_RES_COUNT, &count, sizeof count) || count < 0) count = 0;
    if (count > len) count = len;
    if (count > RR_SHOT_SEGMENTS) count = RR_SHOT_SEGMENTS;

    RR_V3 endpoints[RR_SHOT_SEGMENTS];
    int hits[RR_SHOT_SEGMENTS];
    int segs = 0;
    for (int i = 0; i < count; ++i) {
        const uint64_t ri = data + (uint64_t)i * RR_SHOTRES_STRIDE;
        unsigned char contact = 0;
        RR_V3 end;
        if (!rr_read(ri + RR_SHOTRES_HAS, &contact, 1) || contact > 1) continue;
        if (contact) {
            if (!rr_read(ri + RR_SHOTRES_CONTACT, &end, sizeof end)) continue;
        } else {
            RR_V3 dir;
            if (!rr_read(ri + RR_SHOTRES_DIRECTION, &dir, sizeof dir)) continue;
            const float norm = dir.x*dir.x + dir.y*dir.y + dir.z*dir.z;
            if (!(norm > 0.25f && norm < 2.25f)) continue;
            end = (RR_V3){origin.x + dir.x*RR_SHOT_FALLBACK_M, origin.y + dir.y*RR_SHOT_FALLBACK_M,
                          origin.z + dir.z*RR_SHOT_FALLBACK_M};
        }
        if (tr_plausible(&end)) {
            endpoints[segs] = end;
            hits[segs++] = contact;
        }
    }
    if (!segs) {
        RR_V3 end = { origin.x + aim.x * RR_SHOT_FALLBACK_M,
                      origin.y + aim.y * RR_SHOT_FALLBACK_M,
                      origin.z + aim.z * RR_SHOT_FALLBACK_M };
        const float norm = aim.x*aim.x + aim.y*aim.y + aim.z*aim.z;
        if (norm > 0.25f && norm < 2.25f && tr_plausible(&end)) {
            endpoints[segs] = end;
            hits[segs++] = 0;
        }
    }
    int64_t latest_tick;
    if (world == g_world && rr_read(res + RR_OFF_RES_TICK, &latest_tick, sizeof latest_tick) &&
        latest_tick == tick && segs) {
#ifdef RR_INTERNAL
        int have_muzzle = 0;
        const DWORD now = GetTickCount();
        if (TryAcquireSRWLockShared(&g_muzzle_lock)) {
            if (g_muzzle.world == world && g_muzzle.local == local && now-g_muzzle.at <= 60) {
                origin = g_muzzle.point;
                have_muzzle = 1;
            }
            ReleaseSRWLockShared(&g_muzzle_lock);
        }
        if (!have_muzzle && g_rig_muzzle.at && g_rig_muzzle.world == world &&
            g_rig_muzzle.local == local && now-g_rig_muzzle.at <= 60) {
            origin = g_rig_muzzle.point;
            const float len = hypotf(hypotf(aim.x, aim.z), aim.y);
            if (len > .5f && len < 1.5f) {
                origin.x += aim.x/len*.15f;
                origin.y += aim.y/len*.15f;
                origin.z += aim.z/len*.15f;
            }
            have_muzzle = 1;
        }
        if (!have_muzzle) goto done;
#endif
        g_tr_seen.tick = tick;
        for (int i = 0; i < segs; ++i) tracer_push(&origin, &endpoints[i], hits[i]);
    }
done:
    ReleaseSRWLockExclusive(&g_tr_lock);
}

int game_tracers(RR_TRACER* out, int max, uint32_t life_ms) {
    if (!out || max <= 0) return 0;
    if (!life_ms) life_ms = 600;
    if (!TryAcquireSRWLockShared(&g_tr_lock)) return 0;
    const DWORD now = timeGetTime();
    int n = 0;
    /* Newest first. The ring is chronological, so the first expired slot also
       ends the walk; unused slots carry birth 0 and expire immediately. */
    for (int k = 1; k <= RR_TRACER_CAP && n < max; ++k) {
        const int i = (g_tr_head - k + RR_TRACER_CAP) % RR_TRACER_CAP;
        const uint32_t age = (uint32_t)(now - g_tr_born[i]);
        if (!g_tr_born[i] || age >= life_ms) break;
        out[n] = g_tr_ring[i];
        out[n].age_ms = age;
        ++n;
    }
    ReleaseSRWLockShared(&g_tr_lock);
    return n;
}

typedef struct {
    uint64_t world, model;
    int period;
    int64_t server, simulated, player;
} FRAME_CLOCK;
static int read_frame_clock(FRAME_CLOCK* clock) {
    memset(clock, 0, sizeof(*clock));
    uint64_t world = (uint64_t)InterlockedCompareExchange64(&g_time_world, 0, 0);
    uint64_t model = (uint64_t)InterlockedCompareExchange64(&g_time_model, 0, 0);
    if (!world || world != g_world || !u_is(model, "TimeModel")) return 0;
    unsigned char fields[0x68];
    if (!rr_read(model + 0x10, fields, sizeof(fields))) return 0;
    uint64_t simulated, player;
    memcpy(&clock->period, fields, 4);
    memcpy(&simulated, fields + 0x50, 8);
    memcpy(&player, fields + 0x58, 8);
    memcpy(&clock->server, fields + 0x60, 8);
    if (clock->period < 1 || clock->period > 1000 ||
        !u_is(simulated, "SetNow") || !u_is(player, "SetNow") ||
        !rr_read(simulated + 0x10, &clock->simulated, 8) ||
        !rr_read(player + 0x10, &clock->player, 8) || world != g_world) return 0;
    clock->world = world;
    clock->model = model;
    InterlockedExchange(&g_tick_period, clock->period);
    return 1;
}

static int frame_clock_changed(const FRAME_CLOCK* a, const FRAME_CLOCK* b) {
    return a->world != b->world || a->model != b->model || a->period != b->period ||
           a->server != b->server || a->simulated != b->simulated || a->player != b->player;
}

int game_snapshot(RR_PLAYER* out, int max, RR_CAMERA* cam) {
    static FRAME_CLOCK previous;
    FRAME_CLOCK before, after;
    const BR_Config* cfg = br_get();
    int have_clock = read_frame_clock(&before);
    g_pose_clock_changed = have_clock && frame_clock_changed(&before, &previous);
    int count = snapshot_read(out, max, cam);
    /* A boundary can split entity and camera reads. Retry once with fresh rigs.
       The external reader cannot make Unity's transform writes atomic. */
    if (have_clock && read_frame_clock(&after)) {
        if (frame_clock_changed(&before, &after)) {
            g_pose_clock_changed = 1;
            count = snapshot_read(out, max, cam);
        }
        previous = after;
    } else {
        memset(&previous, 0, sizeof(previous));
        InterlockedExchange(&g_tick_period, 0);
    }
    g_pose_clock_changed = 0;
#ifdef RR_INTERNAL
    if (cfg->show_tracers) tracer_rig_muzzle(out, count, g_world);
#endif
    game_update_tracers(cfg->show_tracers);
    return count;
}

/* Project. */

int game_w2s(const RR_CAMERA* cam, const RR_V3* world, int w, int h,
             float* sx, float* sy) {
    if (!cam || !cam->valid || !world) return 0;

    float fx = cam->fwd.x, fy = cam->fwd.y, fz = cam->fwd.z;
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-6f) return 0;
    fx /= fl; fy /= fl; fz /= fl;

    /* Prefer the camera's own basis (+0x48 right, +0x54 up): deriving it from
     * world up ignores camera roll and skews markers when the view tilts. */
    float rx = cam->right.x, ry = cam->right.y, rz = cam->right.z;
    float rl = sqrtf(rx * rx + ry * ry + rz * rz);
    float vx = cam->up.x, vy = cam->up.y, vz = cam->up.z;
    float vl = sqrtf(vx * vx + vy * vy + vz * vz);
    if (rl < 0.5f || rl > 1.5f || vl < 0.5f || vl > 1.5f) {
        const float ux = 0.0f, uy = 1.0f, uz = 0.0f;
        rx = uy * fz - uz * fy;
        ry = uz * fx - ux * fz;
        rz = ux * fy - uy * fx;
        rl = sqrtf(rx * rx + ry * ry + rz * rz);
        if (rl < 1e-4f) { rx = 1.0f; ry = 0.0f; rz = 0.0f; rl = 1.0f; }
        vx = fy * rz - fz * ry;
        vy = fz * rx - fx * rz;
        vz = fx * ry - fy * rx;
        vl = sqrtf(vx * vx + vy * vy + vz * vz);
        if (vl < 1e-4f) vl = 1.0f;
    }
    rx /= rl; ry /= rl; rz /= rl;
    vx /= vl; vy /= vl; vz /= vl;

    float dx = world->x - cam->pos.x;
    float dy = world->y - cam->pos.y;
    float dz = world->z - cam->pos.z;
    float cx = dx * rx + dy * ry + dz * rz;
    float cy = dx * vx + dy * vy + dz * vz;
    float cz = dx * fx + dy * fy + dz * fz;

    if (cz <= 0.05f) return 0;

    float aspect = (h > 0) ? (float)w / (float)h : 1.7778f;
    float tan_half = tanf(cam->fov * 3.14159265f / 360.0f);
    float ndc_x = (cx / cz) / (tan_half * aspect);
    float ndc_y = (cy / cz) / tan_half;

    if (sx) *sx = (ndc_x * 0.5f + 0.5f) * (float)w;
    if (sy) *sy = (0.5f - ndc_y * 0.5f) * (float)h;
    return (ndc_x > -1.2f && ndc_x < 1.2f && ndc_y > -1.2f && ndc_y < 1.2f) ? 1 : 0;
}

/* Normalize the camera and compute the lens once per mesh projection
 * batch instead of repeating square roots and tanf for every vertex.
 */
void game_w2s_many(const RR_CAMERA* cam, const RR_V3* world, int count, int w, int h, float* xy) {
    if (!world || !xy || count<=0) return;
    for (int i=0;i<count;++i) xy[i*2]=xy[i*2+1]=NAN;
    if (!cam || !cam->valid || w<=0 || h<=0) return;
    float fx=cam->fwd.x,fy=cam->fwd.y,fz=cam->fwd.z;
    float fl=sqrtf(fx*fx+fy*fy+fz*fz); if (fl<1e-6f || !isfinite(fl)) return;
    fx/=fl;fy/=fl;fz/=fl;
    float rx=cam->right.x,ry=cam->right.y,rz=cam->right.z,rl=sqrtf(rx*rx+ry*ry+rz*rz);
    float ux=cam->up.x,uy=cam->up.y,uz=cam->up.z,ul=sqrtf(ux*ux+uy*uy+uz*uz);
    if (rl<0.5f || rl>1.5f || ul<0.5f || ul>1.5f) {
        rx=fz;ry=0;rz=-fx;rl=sqrtf(rx*rx+rz*rz);
        if (rl<1e-4f) { rx=1;ry=rz=0;rl=1; }
        ux=fy*rz-fz*ry;uy=fz*rx-fx*rz;uz=fx*ry-fy*rx;
        ul=sqrtf(ux*ux+uy*uy+uz*uz);if(ul<1e-4f)ul=1;
    }
    rx/=rl;ry/=rl;rz/=rl;ux/=ul;uy/=ul;uz/=ul;
    float t=tanf(cam->fov*3.14159265f/360.0f);
    if (!isfinite(t) || fabsf(t)<1e-6f) return;
    float px=(float)h*0.5f/t,py=px;
    for (int i=0;i<count;++i) {
        float dx=world[i].x-cam->pos.x,dy=world[i].y-cam->pos.y,dz=world[i].z-cam->pos.z;
        float depth=dx*fx+dy*fy+dz*fz;
        if (!isfinite(depth) || depth<=0.05f) continue;
        float x=(dx*rx+dy*ry+dz*rz)/depth, y=(dx*ux+dy*uy+dz*uz)/depth;
        if (isfinite(x) && isfinite(y)) { xy[2*i]=w*0.5f+x*px; xy[2*i+1]=h*0.5f-y*py; }
    }
}





















































