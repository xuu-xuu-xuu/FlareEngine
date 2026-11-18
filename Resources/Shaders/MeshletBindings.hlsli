#ifndef SELF_ENGINE_MESHLET_BINDINGS
#define SELF_ENGINE_MESHLET_BINDINGS

static const uint MeshletAsGroupSize = 32;
static const uint MeshletMsGroupSize = 64;
static const uint MeshletMaxOutputTriangles = 64;
static const uint MeshletMaxOutputVertices = 192;

struct MeshletPayload
{
    uint meshletIndices[MeshletAsGroupSize];
};

struct GpuMeshletItem
{
    uint firstIndex;
    uint triangleCount;
    uint indexStride;
    uint flags;
};

#endif
