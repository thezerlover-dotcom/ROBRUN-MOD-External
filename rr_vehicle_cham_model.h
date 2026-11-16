/* Generated from runtime prefab hulls relative to each vehicle view root, in millimetres. */
#ifndef RR_VEHICLE_CHAM_MODEL_H
#define RR_VEHICLE_CHAM_MODEL_H

typedef struct {
    const char* name;
    const short* positions;
    const unsigned short* triangles;
    int vertices, faces;
} RR_VEHICLE_MESH;

extern const short vehicle_hull_0_positions[];
extern const unsigned short vehicle_hull_0_triangles[];

extern const short vehicle_hull_1_positions[];
extern const unsigned short vehicle_hull_1_triangles[];

extern const short vehicle_hull_2_positions[];
extern const unsigned short vehicle_hull_2_triangles[];

extern const short vehicle_hull_3_positions[];
extern const unsigned short vehicle_hull_3_triangles[];

extern const short vehicle_hull_4_positions[];
extern const unsigned short vehicle_hull_4_triangles[];

extern const short vehicle_hull_5_positions[];
extern const unsigned short vehicle_hull_5_triangles[];

extern const short vehicle_hull_6_positions[];
extern const unsigned short vehicle_hull_6_triangles[];

extern const short vehicle_hull_7_positions[];
extern const unsigned short vehicle_hull_7_triangles[];

extern const short vehicle_hull_8_positions[];
extern const unsigned short vehicle_hull_8_triangles[];

extern const short vehicle_hull_9_positions[];
extern const unsigned short vehicle_hull_9_triangles[];

extern const short vehicle_hull_10_positions[];
extern const unsigned short vehicle_hull_10_triangles[];

extern const short vehicle_hull_11_positions[];
extern const unsigned short vehicle_hull_11_triangles[];

extern const short vehicle_hull_12_positions[];
extern const unsigned short vehicle_hull_12_triangles[];

extern const short vehicle_hull_13_positions[];
extern const unsigned short vehicle_hull_13_triangles[];

#define RR_VEHICLE_MESH_MAX_VERTICES 230
#define RR_VEHICLE_MESH_COUNT 67
extern const RR_VEHICLE_MESH rr_vehicle_meshes[RR_VEHICLE_MESH_COUNT];
#endif
