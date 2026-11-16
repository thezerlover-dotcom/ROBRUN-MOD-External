/* Generated from the offline shared character assets by tools/export_full_rig.py. */
#ifndef RR_CHAM_MODEL_H
#define RR_CHAM_MODEL_H
#include "rr_cham_types.h"
#define RR_CHAM_VERTICES 3854
#define RR_CHAM_TRIANGLES 5688
typedef struct { int parent; float rest[3], rotation[4], scale[3], bind[12], anchor[3], rest_skin[12], joint[3]; const char* name; int required; } RR_CHAM_BONE;
typedef struct { float p[3]; unsigned char bone[4]; float weight[4]; } RR_CHAM_VERTEX;
extern const RR_CHAM_BONE rr_cham_bones[RR_CHAM_BONES];
extern const RR_CHAM_VERTEX rr_cham_vertices[RR_CHAM_VERTICES];
extern const unsigned short rr_cham_triangles[RR_CHAM_TRIANGLES * 3];
#endif
