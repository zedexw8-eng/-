/* rr_game.h - Snapshot types and reader API for ROB RUN.
 *
 * rr_game.c resolves world and camera objects, publishes a validated roster,
 * and samples positions, rigs and world markers. The renderer and aim worker
 * consume caller-owned snapshots instead of traversing live objects themselves.
 *
 * Managed field offsets are in robrun_offsets.h and robrun_sdk_offsets.h.
 * Native Unity layouts and generic payload offsets are separate assumptions
 * that must be checked when the game build changes. Cached heap addresses
 * are hints that require validation in the current process.
 */
#ifndef RR_GAME_H
#define RR_GAME_H

#include <stdint.h>
#include <stddef.h>
#include "game/rig/rr_cham_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RR_MAX_PLAYERS 320
#define RR_NAME_MAX    40

typedef struct { float x, y, z; } RR_V3;
typedef struct { float x, y, z, w; } RR_Q4;

#define RR_MAX_BONES RR_CHAM_BONES
enum { RR_REL_CLAN = 1, RR_REL_POLICE = 2, RR_REL_PARTY = 4 };
enum { RR_LOCAL_CONFIRMED = 1, RR_LOCAL_ESTIMATED = 2 };

/* A player in both groups must pass both switches. Party membership is never
 * hidden here; team checks are an aim-only decision. */
static inline int rr_relationship_allowed(unsigned flags, int clan, int police) {
    unsigned allowed = RR_REL_PARTY | (clan ? RR_REL_CLAN : 0) | (police ? RR_REL_POLICE : 0);
    return (flags & ~allowed) == 0;
}

typedef struct RR_PLAYER {
    float    bone[RR_MAX_BONES][3];  /* reconstructed skeleton joints, world space */
    int      nbones;
    int      bone_group[4];          /* head, neck, chest and pelvis indices */
    int      mesh_bones;             /* joints use live mesh transforms */
    int      full_rig;               /* renderer-order palette; unavailable optional joints are NaN */
    int      cham_valid;             /* skinned shared-body matrices available */
    uint32_t cham_sample_at;         /* timeGetTime of last complete pose */
#ifdef RR_INTERNAL
    uint64_t render_anchor, render_hierarchy;
#endif
    uint64_t cham_rig;               /* identity of the mapped rig's root */
    float    cham_skin[RR_CHAM_BONES][12];
    int      inveh;                  /* seated in a vehicle (entity id valid) */
    int      vehicle_id;             /* replicated vehicle entity ID, or -1  */
    int      occupants;              /* players observed in this vehicle     */
    int      occupants_known;        /* seat data supplied the count          */
    int      vehicle_rendered;       /* position came from the live marker     */
    int      vehicle_cham_valid;     /* vehicle pose available for its hull */
    float    vehicle_pose[12];       /* vehicle pivot to world, row-major 3x4 */
    char     veh[20];                /* vehicle type name when seated        */
    uint64_t addr;                 /* BattleCharacterModel*                 */
    int32_t  id;
    RR_V3    pos;                  /* feet                                  */
    RR_V3    head;                 /* Resolved head joint or estimated head position. */
    float    dist;                 /* metres from the camera                */
    float    rot_yaw;              /* yaw, degrees                          */
    int      dead;
    int      local;                /* RR_LOCAL_* or zero for a remote player */
    unsigned relationships;        /* RR_REL_* relative to the local player  */
    int      valid;
    int      hidden_in_object;    /* 1 confirmed, 2 inferred from last sighting */
    int      is_vehicle;           /* 1 car, 2 helicopter, never a player target */
    int      is_loot;              /* 1 loose, 2 node, 3 container, 4 door, 5 dropped, 6 vehicle loot */
    int      loot_rarity;          /* 0 none, 1 common .. 6 exotic; -1 unknown */
    int      health;               /* -1 unavailable; otherwise raw replicated value */
    int      max_health;           /* 0 when the health model could not be verified */
    int      armor;                /* -1 unknown, 0 none, 1 has armour       */
    int      crouch;               /* -1 unknown, 0 standing, 1 crouched     */
    char     role[20];             /* PlayerRoleDescription.Name            */
    char     weapon[20];           /* WeaponDescription.Name (dp9_m1, ...)   */
    char     name[RR_NAME_MAX];
} RR_PLAYER;

typedef struct RR_CAMERA {
    RR_V3 pos;
    RR_Q4 rot;                     /* x,y,z,w                               */
    RR_V3 fwd;
    RR_V3 right, up;               /* model-space basis (+0x48 / +0x54)     */
    float fov;                     /* degrees                               */
    int   valid;
    uint64_t model_source;         /* camera model used for this snapshot   */
} RR_CAMERA;

/* Resolve the module, classes and current instances; start background
 * discovery if needed. Return 1 when the initial scan finds characters.
 */
int  game_init(void);
/* Read-only diagnostic for the optional UnityEntry discovery path. */
int game_entry_probe(uint64_t* world, uint64_t* battle, uint64_t* camera);
typedef struct {
    unsigned attempts, success, failed, reused_mesh, reused_cham, missing_rig, held_spikes;
    unsigned quat_samples, neck_reversals, left_reversals, right_reversals;
    unsigned pair_samples, pair_changes;
    unsigned both_arms_rest, rest_reversions;
    unsigned rotation_run_return[6];
    unsigned mapping_joints, mapping_ambiguous;
    unsigned ambiguous_by_bone[RR_CHAM_BONES];
    unsigned named_joints, mismatched_joints, mismatch_by_bone[RR_CHAM_BONES];
    unsigned change_mask[8];
    unsigned standing_samples, seated_samples, seated_holds, seated_root_bypass;
    unsigned arm_branch_holds[2];
} RR_POSE_DIAG;
void game_pose_diag_enable(void);
RR_POSE_DIAG game_pose_diag_results(void);
int game_pose_diag_init(uint64_t world_hint);

/* Explicit recalibration (menu / --probe). */
int  game_calibrate(void);

/* Fill out[] with at most max snapshot entries and *cam with the view.
 * Entries can represent players or enabled world markers. Return the entry
 * count. The caller owns the output buffers; cache publication uses a lock.
 */
int  game_snapshot(RR_PLAYER* out, int max, RR_CAMERA* cam);
int  game_refresh_render_camera(RR_CAMERA* cam);
#ifdef RR_INTERNAL
void game_refresh_render_players(RR_PLAYER* players, int count, const RR_CAMERA* cam);
#endif

/* Write the game's own AimTargetModel: character_id is BattleCharacterModel.Id,
 * 0 clears the target. Returns 1 when both fields were written. */
int  game_aim_target(int32_t character_id);
/* Menu cursor ownership: open=1 adds CursorLockKey.SideMenu with
 * CursorLockMode.None through the game's CursorLockModel, open=0 removes it.
 * Returns 0 when the model or the call is unavailable, so the caller can fall
 * back to Win32 cursor handling. */
int  game_cursor_menu(int open);
/* Silent aim: point the local LookDirectionModel at target (the vector
 * ShotArgsSystem uses for the shot ray) from a 4 kHz writer thread that
 * re-states it whenever the look tick overwrites it. on = 0 stops it. Returns
 * nothing; the writer no-ops while models are unresolved. */
void game_silent_aim(int on, RR_V3 target);
/* Camera lock through the CinemachineBrain raw pose. Hold `on` with the point
 * to look at; 0 releases. Blocks for nobody: the worker owns the timing. */
void game_cam_lock(int on, RR_V3 target, float smooth);
/* Pins the brain FOV while degrees > 1; 0 hands it back to the game. */
void game_cam_fov(float degrees);
#ifdef RR_INTERNAL
int game_cam_track(const RR_CAMERA* cam, uint64_t target_id, RR_V3 target, int smooth);
void game_cam_clear(void);
void game_cam_render_state(RR_CAMERA* cam);
#endif
int  game_discovery_active(void);
#ifdef RR_PORT_DIAGNOSTICS
int  game_is_aiming(void);
int  game_shot_event(void);
#endif

/* Human-readable state for the overlay footer and the probe. */
void game_status(char* out, size_t n);

/* Loot vacuum, driven from the addresses game_snapshot's loot entries were
 * validated against.
 *
 *   mode 0 = move each loot position onto the local player (proximity pickup)
 *   mode 1 = set the entry's taken / empty flag
 *   apply 0 = resolve and count only, writing nothing
 *
 * Returns the number of cached entries considered, and reports how many were
 * writable (or would be, with apply 0) and how many were skipped because the
 * cache had aged out. `local_pos_out` may be NULL; it receives the position
 * mode 0 would move loot to (zero when the local player is unknown).
 *
 * Server authority is not modelled here: this changes the client's copy of the
 * loot state, and whether the server accepts any of it depends on the game. */
int  rr_loot_vacuum(int mode, int apply, RR_V3* local_pos_out,
                    int* out_ok, int* out_skipped);

/* World-space -> screen. Uses the camera basis, Unity's left-handed +Z
 * forward convention, vertical FOV in degrees. 1 = on screen. */
int  game_w2s(const RR_CAMERA* cam, const RR_V3* world, int w, int h,
              float* sx, float* sy);
void game_w2s_many(const RR_CAMERA* cam, const RR_V3* world, int count, int w, int h, float* xy);

/* One resolved loot entry, addresses included.
 *
 * These are the objects the ESP itself followed to obtain the name it draws, so
 * dumping them shows what the game actually holds - not a re-resolution that
 * might land somewhere else. `obj` is the node (LootNodeModel), `desc` its
 * description (LocationLootNodeDescription), `loot` the loot description the
 * description's delegate produces, and `inv` the InventoryItemDescription whose
 * name field is the "cash_1" string. */
typedef struct RR_LOOT_ENT {
    uint64_t obj, desc, loot, inv;
    uint64_t pos_addr, flag_addr;
    int32_t  id;
    int      kind, rarity;
    RR_V3    pos;
    char     name[64];
} RR_LOOT_ENT;

/* Copy the published loot cache. Returns the number of entries (0 when the
 * discovery worker has not published anything yet - call game_init() first and
 * poll, the lists appear only once the area is streamed in). */
int rr_loot_snapshot(RR_LOOT_ENT* out, int max);

/* What the loot refresh saw, so "no nodes" can be told apart from "the
 * dictionary is empty" and "the world is wrong". */
typedef struct RR_LOOT_DEBUG {
    uint64_t world;          /* WorldEntitiesModel the cache was built against */
    uint64_t dict_nodes;     /* world+0xB0 - LootNodeModel entries             */
    uint64_t dict_loose;     /* world+0xA0 - RandomLootObjectModel entries     */
    int entries_nodes;       /* its _count, -1 when unreadable                 */
    int entries_loose;
    int cached;              /* entries that survived validation              */
    /* Drop counts distinguish an observed empty/taken node from an invalid
     * read or chain. Invalid reads do not establish that loot is absent.
     */
    int nodes_seen, nodes_empty, nodes_bad;
    int loose_seen, loose_empty, loose_bad;
} RR_LOOT_DEBUG;

/* Rebuild the loot cache on the calling thread.
 *
 * cache_loot() is otherwise reached only from the discovery worker's 2-second
 * timer and only when the overlay's show_loot / show_loot_nodes flags are set,
 * so a diagnostic process can wait forever for a list it never asked for. This
 * drives it directly. Returns the number of published entries. */
int rr_loot_refresh(RR_LOOT_DEBUG* dbg);

/* One drawn shot segment. `a` is the ShotArgsModel origin (the camera-space
 * muzzle the shot ray left from), `b` the ShotResultModel contact point, or
 * the ray extended to its nominal range when the shot found nothing.
 * `age_ms` is measured inside game_tracers, so the renderer does not need a
 * clock of its own. `hit` is 0 for the extended ray. */
typedef struct RR_TRACER {
    RR_V3    a, b;
    uint32_t age_ms;
    int      hit;
} RR_TRACER;

/* Capture on the snapshot worker and copy live segments newest first on draw. */
void game_update_tracers(int enabled);
int game_tracers(RR_TRACER* out, int max, uint32_t life_ms);

/* Address of the local player's BattleCharacterModel, 0 until the roster is
 * published. */
uint64_t rr_local_player(void);

/* Copy the live BattleCharacterEntityView object addresses (managed pointers)
 * out of the battle entities updater's character dictionary. Read-only and
 * callable from any thread, unlike Object.FindObjectsByType. Returns the count,
 * 0 while the updater/world are not resolved yet. */
int  rr_character_views(uint64_t* out, int capacity);

/* Forget every anchor that belongs to the process we were attached to. Call
 * after re-attaching to a relaunched game, before game_init(). */
void game_reattach_reset(void);

/* Offsets discovered at runtime (exposed for the probe printout). */int  game_prim_offset(void);
int  game_dict_offset(void);
int  game_instance_mode(void);
uint64_t game_world_addr(void);
unsigned game_world_generation(void);
int  game_roster_ready(void);

/* Breadcrumb of the step the scene worker or loader is inside right now. A
 * crash handler prints it next to the faulting address, so a death inside a
 * heap sweep reads differently from one inside a managed-object read. */
enum {
    GAME_PHASE_IDLE = 0,
    GAME_PHASE_INIT_MODULE,
    GAME_PHASE_INIT_RUNTIME,
    GAME_PHASE_INIT_CLASSES,
    GAME_PHASE_INIT_ENTRY,
    GAME_PHASE_INIT_SCAN,
    GAME_PHASE_ROSTER_WORLD = 10,
    GAME_PHASE_ROSTER_BATTLE,
    GAME_PHASE_HEAP_SCAN,
    GAME_PHASE_WORKER_IDLE = 20,
    GAME_PHASE_ENTRY_WALK,
    GAME_PHASE_REGION_WALK,
    GAME_PHASE_FAST_SCAN,
    GAME_PHASE_HIDE_CACHE,
    GAME_PHASE_WORLD_FIND,
    GAME_PHASE_VEHICLES,
    GAME_PHASE_LOOT,
    GAME_PHASE_MARKERS,
    GAME_PHASE_BATTLE_SCAN,
    GAME_PHASE_CAMERA_SCAN,
    GAME_PHASE_SNAPSHOT = 40,
    GAME_PHASE_COUNT
};
void game_phase_set(int phase);
int  game_phase_get(void);

void game_scene_debug(uint64_t* battle, uint64_t* updater, int* scanning);
uint64_t game_lens_debug(void);

#ifdef __cplusplus
}
#endif

#endif /* RR_GAME_H */







