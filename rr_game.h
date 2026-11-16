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
#include "rr_cham_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RR_MAX_PLAYERS 320
#define RR_NAME_MAX    40

typedef struct { float x, y, z; } RR_V3;
typedef struct { float x, y, z, w; } RR_Q4;

#define RR_MAX_BONES RR_CHAM_BONES
enum { RR_REL_CLAN = 1, RR_REL_POLICE = 2 };
enum { RR_LOCAL_CONFIRMED = 1, RR_LOCAL_ESTIMATED = 2 };

/* A player in both groups must pass both switches. */
static inline int rr_relationship_allowed(unsigned flags, int clan, int police) {
    unsigned allowed = (clan ? RR_REL_CLAN : 0) | (police ? RR_REL_POLICE : 0);
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

/* Fill out[] with at most max snapshot entries and *cam with the view.
 * Entries can represent players or enabled world markers. Return the entry
 * count. The caller owns the output buffers; cache publication uses a lock.
 */
int  game_snapshot(RR_PLAYER* out, int max, RR_CAMERA* cam);
int  game_refresh_render_camera(RR_CAMERA* cam);
int  game_discovery_active(void);

/* World-space -> screen. Uses the camera basis, Unity's left-handed +Z
 * forward convention, vertical FOV in degrees. 1 = on screen. */
int  game_w2s(const RR_CAMERA* cam, const RR_V3* world, int w, int h,
              float* sx, float* sy);
void game_w2s_many(const RR_CAMERA* cam, const RR_V3* world, int count, int w, int h, float* xy);

/* Forget every anchor that belongs to the process we were attached to. Call
 * after re-attaching to a relaunched game, before game_init(). */
void game_reattach_reset(void);

uint64_t game_world_addr(void);
void game_scene_debug(uint64_t* battle, uint64_t* updater, int* scanning);

#ifdef __cplusplus
}
#endif

#endif /* RR_GAME_H */







