/* rr_hiding.h - Read-only hiding evidence.
 *
 * Voice IDs remain candidates until corroborated by the current hiding
 * object's occupant. Missing reads never mean empty.
 */
#ifndef RR_HIDING_H
#define RR_HIDING_H

#include <stdint.h>
#include <stddef.h>
#include "rr_game.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RR_MAX_HIDE_OBJECTS 512

typedef struct {
    int32_t id;
    RR_V3 pos;                 /* object origin, used for distance/radar */
    RR_V3 anchor;              /* validated world interaction point or fallback */
    int has_interaction, occupant_known, occupant;
} RR_HIDE_OBJECT;

typedef struct {
    int id, voice_known, voice_hide_id;
    uint64_t description, model, data, identity;
    char user[128], name[RR_NAME_MAX]; /* user string stays inside reader */
} RR_HIDE_SLOT;

typedef struct {
    int nobjects, nslots, slots_known, voice_known, voice_participants;
    RR_HIDE_OBJECT objects[RR_MAX_HIDE_OBJECTS];
    RR_HIDE_SLOT slots[RR_MAX_PLAYERS];
} RR_HIDE_SNAPSHOT;

/* Read one hiding object. Return 1 when its description, ID and position are
 * valid. */
int rr_hide_object_read(uint64_t object, int id, RR_HIDE_OBJECT* out);

/* Capture hiding objects, player slots and voice hiding state for the current
 * battle. Return 1 when the snapshot is internally consistent. */
int rr_hide_read(uint64_t battle_model, uint64_t world, RR_HIDE_SNAPSHOT* out);

#ifdef __cplusplus
}
#endif

#endif /* RR_HIDING_H */
