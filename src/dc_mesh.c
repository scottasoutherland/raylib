/*
 * dc_mesh.c — Dreamcast DCMesh Runtime Implementation
 *
 * Phase 2: Strip rendering via GLdc (GL_TRIANGLE_STRIP)
 * Phase 3: Patch E optimized submission (minimal state changes + opaque routing)
 *
 * Drop this file into your raylib src/ directory and add to build.
 * Requires: dcmesh.h, dc_mesh.h, and a Dreamcast target macro.
 */

#if defined(PLATFORM_DREAMCAST) || defined(DREAMCAST)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <GL/gl.h>
#include <GL/glkos.h>   /* glKosDrawMultiStrips (one-call model submission) */

/* These GLdc extensions predate their public glkos.h declarations. Keep exact
 * local prototypes so raylib remains warning-clean with both installed and
 * current project GLdc headers; identical repeated C declarations are valid. */
GLAPI void APIENTRY glKosDrawMultiStrips(const GLint *firsts, const GLsizei *counts, GLsizei n);
GLAPI void APIENTRY glKosDrawTrianglesArrays(GLint first, GLsizei count);

#include "dc_mesh.h"
#include "dc_mesh_color.h"
#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"

/* GLdc stats integration */
#ifdef GLDC_ENABLE_STATS
#include "gldc_stats.h"
#endif

/* -------------------------------------------------------------------
 * Registry — global table mapping IDs to DCMeshData
 * ---------------------------------------------------------------- */
static DCMeshData* dc_registry[DC_MESH_REGISTRY_MAX] = {0};
static unsigned int dc_registry_refs[DC_MESH_REGISTRY_MAX] = {0};
static uint32_t* dc_registry_source_counts[DC_MESH_REGISTRY_MAX] = {0};
static int dc_registry_count = 0;

#define DC_MESH_BATCH_CACHE_MAX 64

typedef struct {
    DCMeshData *data;
    int submesh_index;
    DCVertex *vertices;
    uint32_t vertex_count;
    uint32_t *tint_colors;
    uint32_t last_tint;
    int tint_valid;
} DCMeshBatchCache;

static DCMeshBatchCache dc_batch_caches[DC_MESH_BATCH_CACHE_MAX] = {0};

static void dcBatchFreeCachesForData(DCMeshData *data);
static void dcFreeData(DCMeshData *data);

static int dcRegistryAdd(DCMeshData* data, uint32_t *source_counts, unsigned int refs) {
    if (!data || !source_counts || refs == 0) return -1;
    if (dc_registry_count >= DC_MESH_REGISTRY_MAX) {
        printf("[DCMesh] Registry full (%d max)\n", DC_MESH_REGISTRY_MAX);
        return -1;
    }
    for (int i = 0; i < DC_MESH_REGISTRY_MAX; i++) {
        if (dc_registry[i] == NULL) {
            dc_registry[i] = data;
            dc_registry_refs[i] = refs;
            dc_registry_source_counts[i] = source_counts;
            dc_registry_count++;
            return i;
        }
    }
    return -1;
}

static DCMeshData* dcRegistryGet(unsigned int vaoId) {
    if (!DCMESH_IS_REGISTRY_ID(vaoId)) return NULL;
    int idx = DCMESH_REGISTRY_INDEX(vaoId);
    if (idx < 0 || idx >= DC_MESH_REGISTRY_MAX) return NULL;
    return dc_registry[idx];
}

static void dcRegistryRemove(unsigned int vaoId) {
    if (!DCMESH_IS_REGISTRY_ID(vaoId)) return;
    int idx = DCMESH_REGISTRY_INDEX(vaoId);
    if (idx >= 0 && idx < DC_MESH_REGISTRY_MAX && dc_registry[idx]) {
        dc_registry[idx] = NULL;
        dc_registry_refs[idx] = 0;
        free(dc_registry_source_counts[idx]);
        dc_registry_source_counts[idx] = NULL;
        dc_registry_count--;
    }
}

static uint32_t dcRegistrySourceVertexCount(unsigned int vaoId) {
    if (!DCMESH_IS_REGISTRY_ID(vaoId)) return 0;
    int idx = DCMESH_REGISTRY_INDEX(vaoId);
    int sub = DCMESH_SUBMESH_INDEX(vaoId);
    DCMeshData *data = dcRegistryGet(vaoId);
    if (!data || sub < 0 || sub >= (int)data->submesh_count || !dc_registry_source_counts[idx]) return 0;
    return dc_registry_source_counts[idx][sub];
}

static void dcRegistryRelease(unsigned int vaoId) {
    if (!DCMESH_IS_REGISTRY_ID(vaoId)) return;
    int idx = DCMESH_REGISTRY_INDEX(vaoId);
    if (idx < 0 || idx >= DC_MESH_REGISTRY_MAX || !dc_registry[idx]) return;

    if (dc_registry_refs[idx] > 0) dc_registry_refs[idx]--;
    if (dc_registry_refs[idx] == 0) {
        DCMeshData *data = dc_registry[idx];
        dcRegistryRemove(vaoId);
        dcFreeData(data);
    }
}

/* -------------------------------------------------------------------
 * File loader — reads .dcmesh binary into DCMeshData
 * ---------------------------------------------------------------- */
static int dcReadExact(FILE *f, void *dst, size_t bytes, size_t *remaining) {
    if (bytes > *remaining) return 0;
    if ((bytes > 0) && (fread(dst, 1, bytes, f) != bytes)) return 0;
    *remaining -= bytes;
    return 1;
}

static int dcArrayBytes(uint32_t count, size_t item_size, size_t *bytes) {
    if ((item_size != 0) && ((size_t)count > SIZE_MAX/item_size)) return 0;
    *bytes = (size_t)count*item_size;
    return 1;
}

static DCMeshData* dcLoadFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long end = ftell(f);
    if ((end < 0) || (fseek(f, 0, SEEK_SET) != 0)) { fclose(f); return NULL; }
    size_t remaining = (size_t)end;

    /* Read and validate header */
    DCMeshFileHeader fhdr;
    if (!dcReadExact(f, &fhdr, sizeof(fhdr), &remaining)) { fclose(f); return NULL; }

    if ((fhdr.magic != DCMESH_MAGIC) || (fhdr.version != DCMESH_VERSION) ||
        (fhdr.submesh_count == 0) || (fhdr.submesh_count > 256) ||
        (fhdr.reserved[0] != 0) || (fhdr.reserved[1] != 0) || (fhdr.reserved[2] != 0)) {
        printf("[DCMesh] Invalid file: %s (magic=0x%X ver=%u)\n",
               path, (unsigned)fhdr.magic, (unsigned)fhdr.version);
        fclose(f);
        return NULL;
    }

    /* Allocate runtime structure */
    DCMeshData* data = (DCMeshData*)calloc(1, sizeof(DCMeshData));
    if (!data) { fclose(f); return NULL; }
    data->submesh_count = fhdr.submesh_count;
    data->submeshes = (DCSubmesh*)calloc(fhdr.submesh_count, sizeof(DCSubmesh));
    if (!data->submeshes) goto fail;

    uint64_t total_vertices = 0;
    uint64_t total_strips = 0;

    /* Read each submesh */
    for (uint32_t i = 0; i < fhdr.submesh_count; i++) {
        DCSubmeshHeader shdr;
        if (!dcReadExact(f, &shdr, sizeof(shdr), &remaining)) goto fail;
        if ((shdr.vertex_count == 0) || (shdr.vertex_count > INT_MAX) ||
            (shdr.strip_count == 0) || (shdr.is_opaque > 1)) goto fail;

        size_t vertices_bytes = 0;
        size_t strips_bytes = 0;
        size_t map_bytes = 0;
        if (!dcArrayBytes(shdr.vertex_count, sizeof(DCVertex), &vertices_bytes) ||
            !dcArrayBytes(shdr.strip_count, sizeof(DCStrip), &strips_bytes) ||
            !dcArrayBytes(shdr.vertex_count, sizeof(uint16_t), &map_bytes)) goto fail;
        if ((vertices_bytes > remaining) || (strips_bytes > remaining - vertices_bytes) ||
            (map_bytes > remaining - vertices_bytes - strips_bytes)) goto fail;

        DCSubmesh* sm = &data->submeshes[i];
        sm->material_index = shdr.material_index;
        sm->is_opaque = shdr.is_opaque;
        sm->vertex_count = shdr.vertex_count;
        sm->strip_count = shdr.strip_count;

        /* Read vertices */
        sm->vertices = (DCVertex*)malloc(vertices_bytes);
        if (!sm->vertices) goto fail;
        if (!dcReadExact(f, sm->vertices, vertices_bytes, &remaining)) goto fail;

        /* Read strips */
        if (strips_bytes > 0) {
            sm->strips = (DCStrip*)malloc(strips_bytes);
            if (!sm->strips) goto fail;
            if (!dcReadExact(f, sm->strips, strips_bytes, &remaining)) goto fail;
        }

        /* Read vertex_map (strip vertex -> original vertex index) */
        sm->vertex_map = (uint16_t*)malloc(map_bytes);
        if (!sm->vertex_map) goto fail;
        if (!dcReadExact(f, sm->vertex_map, map_bytes, &remaining)) goto fail;

        for (uint32_t s = 0; s < sm->strip_count; s++) {
            uint32_t first = sm->strips[s].first_vertex;
            uint32_t count = sm->strips[s].vertex_count;
            if ((count < 3) || (count > INT_MAX) || (first > sm->vertex_count) ||
                (count > sm->vertex_count - first)) goto fail;
        }

        total_vertices += shdr.vertex_count;
        total_strips += shdr.strip_count;
        if ((total_vertices > UINT32_MAX) || (total_strips > UINT32_MAX)) goto fail;
    }

    if ((remaining != 0) || (total_vertices != fhdr.total_vertices) ||
        (total_strips != fhdr.total_strips)) goto fail;

    fclose(f);
    printf("[DCMesh] Loaded: %s (%u submeshes, %u verts, %u strips)\n",
           path, (unsigned)fhdr.submesh_count, (unsigned)fhdr.total_vertices,
           (unsigned)fhdr.total_strips);
    return data;

fail:
    printf("[DCMesh] Error reading: %s\n", path);
    /* Cleanup partial load */
    for (uint32_t i = 0; i < data->submesh_count; i++) {
        free(data->submeshes[i].vertices);
        free(data->submeshes[i].strips);
        free(data->submeshes[i].vertex_map);
    }
    free(data->submeshes);
    free(data);
    fclose(f);
    return NULL;
}

static void dcFreeData(DCMeshData* data) {
    if (!data) return;
    dcBatchFreeCachesForData(data);
    for (uint32_t i = 0; i < data->submesh_count; i++) {
        free(data->submeshes[i].vertices);
        free(data->submeshes[i].strips);
        free(data->submeshes[i].vertex_map);
    }
    free(data->submeshes);
    free(data);
}

/* -------------------------------------------------------------------
 * Public API: Load sidecar
 * ---------------------------------------------------------------- */
int dcMeshLoadSidecar(Model *model, const char *modelPath) {
    if (!model || !modelPath || !model->meshes || (model->meshCount <= 0) ||
        (model->meshCount > 256) || !model->materials || (model->materialCount <= 0)) return 0;

    /* LoadModel() already calls this hook. Treat an exact second call as an
       idempotent success; tear down only inconsistent/partial old linkage. */
    DCMeshData *linked_data = NULL;
    int fully_linked = 1;
    int has_link = 0;
    for (int i = 0; i < model->meshCount; i++) {
        DCMeshData *entry = dcRegistryGet(model->meshes[i].vaoId);
        if (entry) {
            has_link = 1;
            if (!linked_data) linked_data = entry;
            if ((entry != linked_data) || (DCMESH_SUBMESH_INDEX(model->meshes[i].vaoId) != (unsigned int)i)) fully_linked = 0;
        } else fully_linked = 0;
    }
    if (has_link && fully_linked && linked_data &&
        ((int)linked_data->submesh_count == model->meshCount)) return 1;
    if (has_link) dcMeshUnloadModel(model);

    /* Build .dcmesh path from model path */
    char dcpath[512];
    const char *slash = strrchr(modelPath, '/');
    const char *backslash = strrchr(modelPath, '\\');
    const char *base = slash;
    if (!base || (backslash && backslash > base)) base = backslash;
    base = base ? base + 1 : modelPath;
    const char *dot = strrchr(base, '.');
    size_t stem_len = dot ? (size_t)(dot - modelPath) : strlen(modelPath);
    static const char suffix[] = ".dcmesh";
    if (stem_len > sizeof(dcpath) - sizeof(suffix)) {
        printf("[DCMesh] Model path is too long: %s\n", modelPath);
        return 0;
    }
    memcpy(dcpath, modelPath, stem_len);
    memcpy(dcpath + stem_len, suffix, sizeof(suffix));

    /* Try to load */
    DCMeshData* data = dcLoadFile(dcpath);
    if (!data) return 0;

    if ((int)data->submesh_count != model->meshCount) {
        printf("[DCMesh] Sidecar/model mesh-count mismatch: %u/%d\n",
               (unsigned)data->submesh_count, model->meshCount);
        dcFreeData(data);
        return 0;
    }

    uint32_t *source_counts = (uint32_t *)calloc(data->submesh_count, sizeof(uint32_t));
    if (!source_counts) { dcFreeData(data); return 0; }
    for (uint32_t i = 0; i < data->submesh_count; i++) {
        DCSubmesh *sm = &data->submeshes[i];
        Mesh *mesh = &model->meshes[i];
        /* DCMesh stores the source glTF material index. raylib reserves
           material slot 0 for its default and maps glTF material N to N+1;
           an unmaterialed primitive stays on slot 0. Accept both encodings so
           validation preserves the established converter/runtime contract. */
        if ((mesh->vertexCount <= 0) ||
            (sm->material_index >= (uint32_t)model->materialCount)) {
            free(source_counts);
            dcFreeData(data);
            return 0;
        }
        int material_matches = 1;
        if (model->meshMaterial) {
            int ray_material = model->meshMaterial[i];
            int dc_material = (int)sm->material_index;
            material_matches = (ray_material == dc_material) ||
                               (ray_material == dc_material + 1);
        }
        if (!material_matches) {
            free(source_counts);
            dcFreeData(data);
            return 0;
        }
        source_counts[i] = (uint32_t)mesh->vertexCount;
        for (uint32_t v = 0; v < sm->vertex_count; v++) {
            if (sm->vertex_map[v] >= source_counts[i]) {
                printf("[DCMesh] Vertex map out of range in submesh %u\n", (unsigned)i);
                free(source_counts);
                dcFreeData(data);
                return 0;
            }
        }
    }

    /* Register */
    int reg_idx = dcRegistryAdd(data, source_counts, (unsigned int)model->meshCount);
    if (reg_idx < 0) {
        free(source_counts);
        dcFreeData(data);
        return 0;
    }

    /* Link each raylib mesh to its corresponding DCMesh submesh.
     * Mesh[0] -> submesh[0], Mesh[1] -> submesh[1], etc.
     * Only link as many as we have submeshes for. */
    int linked = 0;
    for (int i = 0; i < model->meshCount; i++) {
        model->meshes[i].vaoId = DCMESH_MAKE_ID(reg_idx, i);
        linked++;
    }

    printf("[DCMesh] Linked %d/%d meshes to registry[%d] (%u submeshes)\n",
           linked, model->meshCount, reg_idx, (unsigned)data->submesh_count);
    return 1;
}

/* -------------------------------------------------------------------
 * Patch E eligibility check
 * ---------------------------------------------------------------- */
#if ENABLE_PATCH_E
static int dcPatchEEligible(DCSubmesh* sm, Material material) {
    /* Strict scope: opaque, single texture, no weird state */
    if (!sm->is_opaque) return 0;

    /* Must have a valid diffuse texture */
    if (material.maps[MATERIAL_MAP_DIFFUSE].texture.id == 0) return 0;

    /* Material color alpha must be fully opaque */
    if (material.maps[MATERIAL_MAP_DIFFUSE].color.a < 255) return 0;

    return 1;
}
#endif

typedef struct {
    int active;
    GLboolean blend_enabled;
    GLint blend_src;
    GLint blend_dst;
} DCBlendRestore;

static DCBlendRestore dcForceOpaqueBlendOff(int enable) {
    DCBlendRestore restore = {
        .active = enable,
        .blend_enabled = GL_FALSE,
        .blend_src = GL_SRC_ALPHA,
        .blend_dst = GL_ONE_MINUS_SRC_ALPHA
    };

    if (!enable) return restore;

    restore.blend_enabled = glIsEnabled(GL_BLEND);
    if (restore.blend_enabled) {
        glGetIntegerv(GL_BLEND_SRC, &restore.blend_src);
        glGetIntegerv(GL_BLEND_DST, &restore.blend_dst);
        rlDisableColorBlend();
    }

    return restore;
}

static void dcRestoreBlend(DCBlendRestore restore) {
    if (!restore.active || !restore.blend_enabled) return;

    rlEnableColorBlend();
    glBlendFunc(restore.blend_src, restore.blend_dst);
}

typedef struct {
    int active;
    DCMeshBatchCache *cache;
    DCBlendRestore blend_restore;
    int using_base_colors;
} DCMeshBatchContext;

static DCMeshBatchContext dc_batch = {0};

static void dcBatchFreeCache(DCMeshBatchCache *cache) {
    if (!cache) return;
    free(cache->vertices);
    free(cache->tint_colors);
    *cache = (DCMeshBatchCache){0};
}

static void dcBatchFreeCachesForData(DCMeshData *data) {
    for (int i = 0; i < DC_MESH_BATCH_CACHE_MAX; i++) {
        if (dc_batch_caches[i].data == data) {
            if (dc_batch.active && dc_batch.cache == &dc_batch_caches[i])
                dcMeshBatchEnd();
            else if (dc_batch.cache == &dc_batch_caches[i])
                dc_batch = (DCMeshBatchContext){0};
            dcBatchFreeCache(&dc_batch_caches[i]);
        }
    }
}

static int dcBatchTriangleVertexCount(const DCSubmesh *sm, uint32_t *result) {
    uint64_t count = 0;
    for (uint32_t i = 0; i < sm->strip_count; i++) {
        if (sm->strips[i].vertex_count >= 3)
            count += ((uint64_t)sm->strips[i].vertex_count - 2u)*3u;
        if (count > UINT32_MAX) return 0;
    }
    *result = (uint32_t)count;
    return 1;
}

static int dcBatchBuildCache(DCMeshBatchCache *cache, DCMeshData *data, int submesh_index) {
    DCSubmesh *sm = &data->submeshes[submesh_index];
    uint32_t vertex_count = 0;
    if (!dcBatchTriangleVertexCount(sm, &vertex_count) || (vertex_count == 0) ||
        ((size_t)vertex_count > SIZE_MAX/sizeof(DCVertex)) ||
        ((size_t)vertex_count > SIZE_MAX/sizeof(uint32_t))) return 0;

    DCVertex *vertices = (DCVertex *)malloc(vertex_count * sizeof(DCVertex));
    uint32_t *colors = (uint32_t *)malloc(vertex_count * sizeof(uint32_t));
    if (!vertices || !colors) {
        free(vertices);
        free(colors);
        return 0;
    }

    uint32_t out = 0;
    for (uint32_t s = 0; s < sm->strip_count; s++) {
        const DCStrip *strip = &sm->strips[s];
        for (uint32_t j = 2; j < strip->vertex_count; j++) {
            uint32_t a = strip->first_vertex + j - 2;
            uint32_t b = strip->first_vertex + j - 1;
            uint32_t c = strip->first_vertex + j;

            if (((j - 2) & 1) != 0) {
                uint32_t t = a;
                a = b;
                b = t;
            }

            vertices[out++] = sm->vertices[a];
            vertices[out++] = sm->vertices[b];
            vertices[out++] = sm->vertices[c];
        }
    }

    cache->data = data;
    cache->submesh_index = submesh_index;
    cache->vertices = vertices;
    cache->vertex_count = vertex_count;
    cache->tint_colors = colors;
    cache->tint_valid = 0;
    return 1;
}

static DCMeshBatchCache *dcBatchGetCache(DCMeshData *data, int submesh_index) {
    DCMeshBatchCache *empty = NULL;

    for (int i = 0; i < DC_MESH_BATCH_CACHE_MAX; i++) {
        DCMeshBatchCache *cache = &dc_batch_caches[i];
        if (cache->data == data && cache->submesh_index == submesh_index)
            return cache;
        if (!cache->data && !empty)
            empty = cache;
    }

    if (!empty) return NULL;
    if (!dcBatchBuildCache(empty, data, submesh_index)) return NULL;
    return empty;
}

static void dcBatchFillTintColors(DCMeshBatchCache *cache, Color tint) {
    /* Per-instance draws re-tint the whole buffer even when the tint repeats
       (enemy formations share shades): skip when unchanged since the last
       fill. Invalidated by color syncs and cache rebuilds. */
    uint32_t packed = ((uint32_t)tint.r << 24) | ((uint32_t)tint.g << 16) |
                      ((uint32_t)tint.b << 8) | (uint32_t)tint.a;
    if (cache->tint_valid && cache->last_tint == packed) return;
    cache->last_tint = packed;
    cache->tint_valid = 1;
    for (uint32_t i = 0; i < cache->vertex_count; i++) {
        const unsigned char *src = (const unsigned char *)&cache->vertices[i].color;
        unsigned char *dst = (unsigned char *)&cache->tint_colors[i];
        dst[0] = (unsigned char)(((unsigned int)src[0] * tint.b) / 255);
        dst[1] = (unsigned char)(((unsigned int)src[1] * tint.g) / 255);
        dst[2] = (unsigned char)(((unsigned int)src[2] * tint.r) / 255);
        dst[3] = (unsigned char)(((unsigned int)src[3] * tint.a) / 255);
    }
}

/* -------------------------------------------------------------------
 * Public API: Explicit one-setup batch draw for repeated instances
 * ---------------------------------------------------------------- */
int dcMeshBatchBegin(Model model, int materialIndex) {
    if (dc_batch.active) dcMeshBatchEnd();
    if (materialIndex < 0 || materialIndex >= model.materialCount) return 0;
    if (!model.meshes || !model.materials) return 0;

    DCMeshData *data = NULL;
    int submesh_index = -1;
    int match_count = 0;

    for (int i = 0; i < model.meshCount; i++) {
        int mesh_material = model.meshMaterial ? model.meshMaterial[i] : 0;
        if (mesh_material != materialIndex) continue;

        data = dcRegistryGet(model.meshes[i].vaoId);
        if (!data) continue;

        submesh_index = DCMESH_SUBMESH_INDEX(model.meshes[i].vaoId);
        if (submesh_index >= 0 && submesh_index < (int)data->submesh_count) {
            match_count++;
            if (match_count > 1) {
                /* A one-cache batch cannot omit a second same-material mesh.
                   Preserve the public result through the ordinary draw path. */
                return 0;
            }
            continue;
        }

        data = NULL;
        submesh_index = -1;
    }

    if (!data || submesh_index < 0) return 0;

    DCMeshBatchCache *cache = dcBatchGetCache(data, submesh_index);
    if (!cache || !cache->vertices || cache->vertex_count == 0) return 0;

    Material material = model.materials[materialIndex];
    DCSubmesh *sm = &data->submeshes[submesh_index];

#if ENABLE_PATCH_E
    int use_patchE = dcPatchEEligible(sm, material);
#else
    int use_patchE = 0;
#endif

    dc_batch.blend_restore = dcForceOpaqueBlendOff(use_patchE);
    dc_batch.cache = cache;
    dc_batch.active = 1;
    dc_batch.using_base_colors = 1;

    const GLsizei stride = sizeof(DCVertex);
    const DCVertex *buf = cache->vertices;

    rlEnableTexture(material.maps[MATERIAL_MAP_DIFFUSE].texture.id);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);

    glVertexPointer(3, GL_FLOAT, stride, &buf[0].x);
    glTexCoordPointer(2, GL_FLOAT, stride, &buf[0].u);
    glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, stride, &buf[0].color);

    return 1;
}

void dcMeshBatchDraw(Matrix transform, Color tint) {
    if (!dc_batch.active || !dc_batch.cache) return;

    DCMeshBatchCache *cache = dc_batch.cache;
    const GLsizei stride = sizeof(DCVertex);
    const DCVertex *buf = cache->vertices;
    int needs_tint = tint.r != 255 || tint.g != 255 || tint.b != 255 || tint.a != 255;

    if (needs_tint) {
        dcBatchFillTintColors(cache, tint);
        glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, 0, cache->tint_colors);
        dc_batch.using_base_colors = 0;
    } else if (!dc_batch.using_base_colors) {
        glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, stride, &buf[0].color);
        dc_batch.using_base_colors = 1;
    }

    rlPushMatrix();
    rlMultMatrixf(MatrixToFloat(transform));
    /* Fused-lane submission (2026-07-15): the GL_TRIANGLES call rode GLdc's
     * generic generator (the interleaved/BGRA layout misses the PUC dispatch).
     * Same records, PUC-grade writer. */
    glKosDrawTrianglesArrays(0, (GLsizei)cache->vertex_count);
    rlPopMatrix();
}

void dcMeshBatchEnd(void) {
    if (!dc_batch.active) return;

    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    rlDisableTexture();
    dcRestoreBlend(dc_batch.blend_restore);
    dc_batch = (DCMeshBatchContext){0};
}

/* -------------------------------------------------------------------
 * Phase 2: Strip rendering via GLdc
 *
 * Sets up client arrays once for all strips in a submesh,
 * then submits each strip as GL_TRIANGLE_STRIP. GLdc's
 * genTriangleStrip() is trivial (one EOL flag), so this
 * avoids the per-triangle EOL overhead of GL_TRIANGLES.
 *
 * Phase 3 (Patch E): When eligible, route the submesh to GLdc's opaque
 * list by temporarily disabling blending, then restore caller blend state.
 * ---------------------------------------------------------------- */
static void dcDrawSubmesh(DCSubmesh* sm, Material material, Matrix transform) {
    if (sm->vertex_count == 0 || sm->strip_count == 0) return;

    const GLsizei stride = sizeof(DCVertex);
    const DCVertex* buf = sm->vertices;

#if ENABLE_PATCH_E
    int use_patchE = dcPatchEEligible(sm, material);
#else
    int use_patchE = 0;
#endif

#ifdef GLDC_ENABLE_STATS
    if (use_patchE) GLDC_STAT_INC(patchE_hits);
    else GLDC_STAT_INC(patchE_fallbacks);
    GLDC_STAT_ADD(strip_count, sm->strip_count);
    GLDC_STAT_ADD(strip_vertices_total, sm->vertex_count);
#endif

    DCBlendRestore blend_restore = dcForceOpaqueBlendOff(use_patchE);

    /* Bind texture */
    unsigned int texId = material.maps[MATERIAL_MAP_DIFFUSE].texture.id;
    rlEnableTexture(texId);

    /* Set up client arrays — one setup for all strips in this submesh.
     * Layout matches GLdc fast-path requirements:
     *   vertex:  3 x GL_FLOAT
     *   uv:      2 x GL_FLOAT
     *   color:   fixed-function current color (no array; set via rlColor4ub) */
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);

    glVertexPointer(3, GL_FLOAT, stride, &buf[0].x);
    glTexCoordPointer(2, GL_FLOAT, stride, &buf[0].u);

    /* Push model transform */
    rlPushMatrix();
    rlMultMatrixf(MatrixToFloat(transform));

    /* Set material tint color */
    rlColor4ub(material.maps[MATERIAL_MAP_DIFFUSE].color.r,
               material.maps[MATERIAL_MAP_DIFFUSE].color.g,
               material.maps[MATERIAL_MAP_DIFFUSE].color.b,
               material.maps[MATERIAL_MAP_DIFFUSE].color.a);

    /* Submit all strips in ONE GLdc call (glKosDrawMultiStrips): the old
     * per-strip glDrawArrays loop paid list bookkeeping + a full XMTRX MVP
     * concat per strip (F22: 192 strips/frame) and routed through GLdc's
     * generic generator. Chunked to bound the stack arrays. */
    {
        enum { DC_STRIP_CHUNK = 64 };
        GLint   firsts[DC_STRIP_CHUNK];
        GLsizei counts[DC_STRIP_CHUNK];
        uint32_t si = 0;
        while (si < sm->strip_count) {
            GLsizei nchunk = 0;
            while (si < sm->strip_count && nchunk < DC_STRIP_CHUNK) {
                firsts[nchunk] = (GLint)sm->strips[si].first_vertex;
                counts[nchunk] = (GLsizei)sm->strips[si].vertex_count;
                ++nchunk;
                ++si;
            }
            glKosDrawMultiStrips(firsts, counts, nchunk);
        }
    }

    rlPopMatrix();

    /* Tear down client state */
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    rlDisableTexture();
    dcRestoreBlend(blend_restore);
}

/* -------------------------------------------------------------------
 * Public API: Draw mesh with DCMesh fast path
 * ---------------------------------------------------------------- */
void dcMeshDraw(Mesh mesh, Material material, Matrix transform) {
    DCMeshData* data = dcRegistryGet(mesh.vaoId);
    int sub_idx = DCMESH_SUBMESH_INDEX(mesh.vaoId);

    if (!data || !ENABLE_STRIPS || (sub_idx < 0) || (sub_idx >= (int)data->submesh_count)) {
        /* No strip data or strips disabled — standard DrawMesh path */
        unsigned int saved = mesh.vaoId;
        mesh.vaoId = 0;
        DrawMesh(mesh, material, transform);
        mesh.vaoId = saved;
        return;
    }

    /* Draw only the submesh corresponding to this mesh */
    dcDrawSubmesh(&data->submeshes[sub_idx], material, transform);
}

/* -------------------------------------------------------------------
 * Public API: Unload model's DCMesh data
 * ---------------------------------------------------------------- */
void dcMeshUnloadModel(Model *model) {
    if (!model || !model->meshes || (model->meshCount <= 0)) return;

    for (int i = 0; i < model->meshCount; i++) {
        unsigned int vaoId = model->meshes[i].vaoId;
        if (DCMESH_IS_REGISTRY_ID(vaoId)) {
            dcRegistryRelease(vaoId);
            model->meshes[i].vaoId = 0;
        }
    }
}

void dcMeshUnloadMesh(Mesh *mesh) {
    if (!mesh || !DCMESH_IS_REGISTRY_ID(mesh->vaoId)) return;
    dcRegistryRelease(mesh->vaoId);
    mesh->vaoId = 0;
}

/* -------------------------------------------------------------------
 * Public API: Query and diagnostics
 * ---------------------------------------------------------------- */
int dcMeshHasStripData(Mesh mesh) {
    return dcRegistryGet(mesh.vaoId) != NULL;
}

void dcMeshPrintRegistryStats(void) {
    printf("[DCMesh] Registry: %d/%d entries\n", dc_registry_count, DC_MESH_REGISTRY_MAX);
    for (int i = 0; i < DC_MESH_REGISTRY_MAX; i++) {
        if (dc_registry[i]) {
            DCMeshData* d = dc_registry[i];
            uint32_t total_v = 0, total_s = 0;
            for (uint32_t j = 0; j < d->submesh_count; j++) {
                total_v += d->submeshes[j].vertex_count;
                total_s += d->submeshes[j].strip_count;
            }
            printf("  [%d] %u submeshes, %u vertices, %u strips\n",
                   i, (unsigned)d->submesh_count, (unsigned)total_v,
                   (unsigned)total_s);
        }
    }
}

/* -------------------------------------------------------------------
 * Internal: Sync all vertex data from raylib mesh to DCMesh strips
 *
 * Called automatically by the UploadMesh hook. Syncs positions and
 * colors from the raylib mesh arrays into dcmesh strip vertices
 * using vertex_map for correct index mapping.
 *
 * This is why game code doesn't need to call dcMeshRecenterGeometry
 * or dcMeshSyncColors — UploadMesh handles it transparently.
 * ---------------------------------------------------------------- */
static void dcMeshSyncFromRaylib(Mesh *mesh) {
    if (!mesh) return;

    DCMeshData* data = dcRegistryGet(mesh->vaoId);
    if (!data) return;

    int sub_idx = DCMESH_SUBMESH_INDEX(mesh->vaoId);
    if (sub_idx < 0 || sub_idx >= (int)data->submesh_count) return;

    DCSubmesh* sm = &data->submeshes[sub_idx];
    if (!sm->vertex_map) return;

    int rl_vc = mesh->vertexCount;
    uint32_t source_vc = dcRegistrySourceVertexCount(mesh->vaoId);
    if ((rl_vc <= 0) || (source_vc == 0) || ((uint32_t)rl_vc < source_vc)) return;

    /* Sync positions if available */
    if (mesh->vertices) {
        float *verts = mesh->vertices;
        for (uint32_t v = 0; v < sm->vertex_count; v++) {
            int si = sm->vertex_map[v];
            sm->vertices[v].x = verts[si * 3 + 0];
            sm->vertices[v].y = verts[si * 3 + 1];
            sm->vertices[v].z = verts[si * 3 + 2];
        }
    }

    /* Sync colors in the Dreamcast-native packed BGRA producer layout.
       Raylib allocations are word-aligned; custom public Mesh buffers need
       the alignment-safe fallback. The standard hot path remains one word
       load/store and neither path performs a channel conversion. */
    if (mesh->colors) {
        const uint32_t n = sm->vertex_count;
        if ((((uintptr_t)mesh->colors) & (sizeof(uint32_t) - 1u)) == 0u) {
            const uint32_t *cols = (const uint32_t *)mesh->colors;
            for (uint32_t v = 0; v < n; v++) {
                int si = sm->vertex_map[v];
                sm->vertices[v].color = cols[si];
            }
        } else {
            const unsigned char *cols = mesh->colors;
            for (uint32_t v = 0; v < n; v++) {
                const unsigned char *src = cols + (size_t)sm->vertex_map[v]*4u;
                sm->vertices[v].color = dcMeshUnalignedColorWord(src);
            }
        }
    }

    dcBatchFreeCachesForData(data);
}

/* -------------------------------------------------------------------
 * Public API: colors-only sync (2026-07-15, [PROF][DC] ply=2.84ms hunt)
 *
 * The per-frame CPU lighting path only rewrites mesh->colors; the old route
 * (UploadMesh -> dcMeshSyncFromRaylib) also re-scattered UNCHANGED positions,
 * and freed any batch caches for the data. For the normal player draw
 * (dcDrawSubmesh, no batch cache) this is a positions-scatter reduction
 * (~0.3ms measured); for batch users it additionally keeps the cache alive
 * by patching its colors in place (same expansion walk as dcBatchBuildCache).
 * ---------------------------------------------------------------- */
void dcMeshSyncColors(Mesh *mesh) {
    if (!mesh || !DCMESH_IS_REGISTRY_ID(mesh->vaoId) || !mesh->colors) return;

    DCMeshData *data = dcRegistryGet(mesh->vaoId);
    if (!data) return;

    int sub_idx = DCMESH_SUBMESH_INDEX(mesh->vaoId);
    if (sub_idx < 0 || sub_idx >= (int)data->submesh_count) return;

    DCSubmesh *sm = &data->submeshes[sub_idx];
    if (!sm->vertex_map) return;

    const int rl_vc = mesh->vertexCount;
    const uint32_t source_vc = dcRegistrySourceVertexCount(mesh->vaoId);
    if ((rl_vc <= 0) || (source_vc == 0) || ((uint32_t)rl_vc < source_vc)) return;
    /* Hoist both the loop bound and alignment decision. Normal raylib colors
       are aligned and retain the single native packed-word copy. A custom
       byte buffer may be unaligned; assemble the identical little-endian word
       only on that cold path so SH4 never performs an unaligned uint32 load. */
    const uint32_t n = sm->vertex_count;
    if ((((uintptr_t)mesh->colors) & (sizeof(uint32_t) - 1u)) == 0u) {
        const uint32_t *colw = (const uint32_t *)mesh->colors;
        for (uint32_t v = 0; v < n; v++) {
            int si = sm->vertex_map[v];
            sm->vertices[v].color = colw[si];
        }
    } else {
        const unsigned char *cols = mesh->colors;
        for (uint32_t v = 0; v < n; v++) {
            const unsigned char *src = cols + (size_t)sm->vertex_map[v]*4u;
            sm->vertices[v].color = dcMeshUnalignedColorWord(src);
        }
    }

    /* Patch the live triangle-expanded cache(s): same walk as dcBatchBuildCache,
       colors only. No frees, no mallocs, no re-expansion. */
    for (int ci = 0; ci < DC_MESH_BATCH_CACHE_MAX; ci++) {
        DCMeshBatchCache *cache = &dc_batch_caches[ci];
        if (cache->data != data || cache->submesh_index != sub_idx || !cache->vertices) continue;
        uint32_t out = 0;
        for (uint32_t s = 0; s < sm->strip_count; s++) {
            const DCStrip *strip = &sm->strips[s];
            for (uint32_t j = 2; j < strip->vertex_count; j++) {
                uint32_t a = strip->first_vertex + j - 2;
                uint32_t b = strip->first_vertex + j - 1;
                uint32_t c = strip->first_vertex + j;
                if (((j - 2) & 1) != 0) { uint32_t t = a; a = b; b = t; }
                cache->vertices[out++].color = sm->vertices[a].color;
                cache->vertices[out++].color = sm->vertices[b].color;
                cache->vertices[out++].color = sm->vertices[c].color;
            }
        }
        cache->tint_valid = 0;   /* base colors changed: stale tint buffer */
    }
}

/* -------------------------------------------------------------------
 * Public API: UploadMesh hook — called from patched UploadMesh
 *
 * If mesh has dcmesh data, syncs raylib vertex/color data to strips
 * and returns 1 (caller should skip normal UploadMesh body).
 * If no dcmesh data, returns 0 (caller runs normally).
 * ---------------------------------------------------------------- */
int dcMeshHandleUpload(Mesh *mesh, bool dynamic) {
    (void)dynamic;
    if (!mesh || !DCMESH_IS_REGISTRY_ID(mesh->vaoId)) return 0;

    dcMeshSyncFromRaylib(mesh);
    return 1;  /* Handled — skip normal UploadMesh */
}

/* -------------------------------------------------------------------
 * Public API: Recenter DCMesh geometry to match recenter_model_geometry()
 *
 * Uses -= to match raylib's recenter convention. Pass the same
 * (dx, dy, dz) values you pass to recenter_model_geometry().
 * ---------------------------------------------------------------- */
void dcMeshRecenterGeometry(Model *model, float offsetX, float offsetY, float offsetZ) {
    if (!model || !model->meshes || (model->meshCount <= 0)) return;

    for (int i = 0; i < model->meshCount; i++) {
        DCMeshData* data = dcRegistryGet(model->meshes[i].vaoId);
        if (!data) continue;

        int sub_idx = DCMESH_SUBMESH_INDEX(model->meshes[i].vaoId);
        if (sub_idx < 0 || sub_idx >= (int)data->submesh_count) continue;

        DCSubmesh* sm = &data->submeshes[sub_idx];
        for (uint32_t v = 0; v < sm->vertex_count; v++) {
            sm->vertices[v].x -= offsetX;
            sm->vertices[v].y -= offsetY;
            sm->vertices[v].z -= offsetZ;
        }

        dcBatchFreeCachesForData(data);
    }
}

/* -------------------------------------------------------------------
 * Public API: compatibility UploadMesh wrapper
 *
 * Tagged meshes take UploadMesh's DCMesh synchronization hook; ordinary
 * meshes take the regular upload path.
 * ---------------------------------------------------------------- */
void dcMeshUploadSafe(Mesh *mesh, bool dynamic) {
    if (!mesh) return;
    UploadMesh(mesh, dynamic);
}

#endif /* PLATFORM_DREAMCAST || DREAMCAST */
