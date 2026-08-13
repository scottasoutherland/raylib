/*
 * dcmesh.h — DCMesh binary format definition
 *
 * Shared between the PC-side offline converter (glb_to_dcmesh) and the
 * Dreamcast runtime loader. Defines the .dcmesh file format, vertex
 * layout, and strip structures.
 *
 * The format is designed for zero-overhead loading on Dreamcast:
 *   - Vertex layout matches GLdc's Vertex struct and PVR hardware
 *   - Strips are pre-computed offline via meshoptimizer
 *   - No index buffers needed at runtime
 *   - Pre-expanded vertices for direct array submission
 */

#ifndef DCMESH_H
#define DCMESH_H

#include <stdint.h>

/* -------------------------------------------------------------------
 * File format magic and version
 * ---------------------------------------------------------------- */
#define DCMESH_MAGIC      0x4D434431  /* Little-endian bytes: "1DCM" */
#define DCMESH_VERSION    2           /* v2: added vertex_map */

/* -------------------------------------------------------------------
 * On-disk vertex format — 24 bytes, matches rlgl batcher format
 *
 * Position: 3 x float (12 bytes)
 * Texcoord: 2 x float (8 bytes)
 * Color:    BGRA packed uint32_t (4 bytes)
 *
 * Note: This is the MODEL-SPACE vertex. Transform happens at runtime.
 * ---------------------------------------------------------------- */
typedef struct {
    float x, y, z;          /* 12 bytes — model-space position */
    float u, v;             /*  8 bytes — texture coordinate */
    uint32_t color;         /*  4 bytes — packed BGRA color */
} DCVertex;                 /* 24 bytes total */

/* -------------------------------------------------------------------
 * Strip descriptor — defines a contiguous run of strip vertices
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t first_vertex;  /* Index of first vertex in the mesh vertex array */
    uint32_t vertex_count;  /* Number of vertices in this strip */
} DCStrip;

/* -------------------------------------------------------------------
 * Per-submesh header — one material = one submesh
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t material_index;    /* Source glTF index; raylib loads it at index + 1 */
    uint32_t vertex_count;      /* Total vertices in this submesh */
    uint32_t strip_count;       /* Number of triangle strips */
    uint32_t is_opaque;         /* 1 = opaque (Patch E eligible), 0 = translucent */
    /* Followed in file by:
     *   DCVertex  vertices[vertex_count]
     *   DCStrip   strips[strip_count]
     *   uint16_t  vertex_map[vertex_count]  -- maps strip vertex -> source vertex
     */
} DCSubmeshHeader;

/* -------------------------------------------------------------------
 * File header
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t magic;             /* DCMESH_MAGIC */
    uint32_t version;           /* DCMESH_VERSION */
    uint32_t submesh_count;     /* Number of submeshes in the file */
    uint32_t total_vertices;    /* Total vertices across all submeshes */
    uint32_t total_strips;      /* Total strips across all submeshes */
    uint32_t reserved[3];       /* Future use, must be zero */
} DCMeshFileHeader;

/* -------------------------------------------------------------------
 * Runtime submesh — loaded into memory
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t material_index;
    uint32_t is_opaque;
    uint32_t vertex_count;
    uint32_t strip_count;
    DCVertex *vertices;         /* Allocated vertex array */
    DCStrip  *strips;           /* Allocated strip array */
    uint16_t *vertex_map;       /* Maps strip vertex -> original vertex index */
    /* Runtime-only tint scratch (not serialized): base colors * material tint,
       used to keep the color array bound so GLdc stays on its fast path. */
    uint32_t *tint_colors;
    uint32_t last_tint;
    int tint_valid;
} DCSubmesh;

/* -------------------------------------------------------------------
 * Runtime mesh — the complete loaded .dcmesh
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t submesh_count;
    DCSubmesh *submeshes;       /* Allocated submesh array */
} DCMeshData;

/* -------------------------------------------------------------------
 * Registry handle — used to associate raylib Mesh with DCMeshData
 *
 * On Dreamcast GL 1.1, Mesh.vaoId is unused (VAOs are GL 3.3+).
 * We encode both registry index AND submesh index in vaoId:
 *   Bits 31-24: 0xDC prefix (magic tag)
 *   Bits 23-8:  registry index (up to 65535 entries)
 *   Bits 7-0:   submesh index (up to 256 submeshes per entry)
 * ---------------------------------------------------------------- */
#define DCMESH_REGISTRY_MAGIC_BASE  0xDC000000
#define DCMESH_IS_REGISTRY_ID(id)   (((id) & 0xFF000000) == DCMESH_REGISTRY_MAGIC_BASE)
#define DCMESH_REGISTRY_INDEX(id)   (((id) & 0x00FFFF00) >> 8)
#define DCMESH_SUBMESH_INDEX(id)    ((id) & 0x000000FF)
#define DCMESH_MAKE_ID(reg, sub)    (DCMESH_REGISTRY_MAGIC_BASE | (((reg) & 0xFFFF) << 8) | ((sub) & 0xFF))

#endif /* DCMESH_H */
