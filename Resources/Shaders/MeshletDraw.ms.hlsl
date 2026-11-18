#include "MeshletBindings.hlsli"
#include "PbrBindings.hlsli"

cbuffer MeshletDispatchConstants : register(b0, space1)
{
    uint firstMeshlet;
    uint dispatchMeshletCount;
    uint vertexByteOffset;
    uint indexElementOffset;
};

StructuredBuffer<GpuMeshletItem> meshletItems : register(t1, space1);
ByteAddressBuffer sourceVertices : register(t2, space1);
ByteAddressBuffer sourceIndices : register(t3, space1);

struct MSVertexOut
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};

groupshared uint sHashKey[256];
groupshared uint sHashVal[256];
groupshared uint sUniqueSourceVertex[MeshletMaxOutputVertices];
groupshared uint sUniqueCount;

uint LoadSourceIndex(uint elementIndex, uint indexStride)
{
    if (indexStride == 2)
    {
        uint word = sourceIndices.Load((elementIndex & ~1u) * 2u);
        return (elementIndex & 1u) != 0 ? (word >> 16) : (word & 0xFFFFu);
    }
    return sourceIndices.Load(elementIndex * 4u);
}

uint HashVertexKey(uint key)
{
    uint h = key * 0x9E3779B1u;
    return (h ^ (h >> 16)) & 255u;
}

void InsertUniqueVertex(uint key)
{
    uint slot = HashVertexKey(key);
    [loop]
    for (uint probe = 0u; probe < 256u; ++probe)
    {
        uint cur = sHashKey[slot];
        if (cur == key) return;
        if (cur == 0xFFFFFFFFu)
        {
            uint prev = 0u;
            InterlockedCompareExchange(sHashKey[slot], 0xFFFFFFFFu, key, prev);
            if (prev == 0xFFFFFFFFu)
            {
                uint localIdx = 0u;
                InterlockedAdd(sUniqueCount, 1u, localIdx);
                sUniqueSourceVertex[localIdx] = key;
                sHashVal[slot] = localIdx;
                return;
            }
            if (prev == key) return;
        }
        slot = (slot + 1u) & 255u;
    }
}

uint LookupUniqueVertex(uint key)
{
    uint slot = HashVertexKey(key);
    [loop]
    for (uint probe = 0u; probe < 256u; ++probe)
    {
        if (sHashKey[slot] == key) return sHashVal[slot];
        slot = (slot + 1u) & 255u;
    }
    return 0u;
}

[numthreads(MeshletMsGroupSize, 1, 1)]
[outputtopology("triangle")]
void MSMain(
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID,
    in payload MeshletPayload incoming,
    out vertices MSVertexOut outVertices[MeshletMaxOutputVertices],
    out indices uint3 outTriangles[MeshletMaxOutputTriangles])
{
    uint meshletIndex = incoming.meshletIndices[gid];
    GpuMeshletItem item = meshletItems[meshletIndex];
    uint triCount = min(item.triangleCount, MeshletMaxOutputTriangles);

    uint baseHash = gtid * 4u;
    sHashKey[baseHash + 0u] = 0xFFFFFFFFu;
    sHashKey[baseHash + 1u] = 0xFFFFFFFFu;
    sHashKey[baseHash + 2u] = 0xFFFFFFFFu;
    sHashKey[baseHash + 3u] = 0xFFFFFFFFu;
    if (gtid == 0u) sUniqueCount = 0u;
    GroupMemoryBarrierWithGroupSync();

    uint3 srcTri = uint3(0u, 0u, 0u);
    if (gtid < triCount)
    {
        uint elem = indexElementOffset + item.firstIndex + gtid * 3u;
        srcTri.x = LoadSourceIndex(elem + 0u, item.indexStride);
        srcTri.y = LoadSourceIndex(elem + 1u, item.indexStride);
        srcTri.z = LoadSourceIndex(elem + 2u, item.indexStride);
        InsertUniqueVertex(srcTri.x);
        InsertUniqueVertex(srcTri.y);
        InsertUniqueVertex(srcTri.z);
    }
    GroupMemoryBarrierWithGroupSync();

    uint vertCount = min(sUniqueCount, MeshletMaxOutputVertices);
    SetMeshOutputCounts(vertCount, triCount);

    for (uint localVertex = gtid; localVertex < vertCount; localVertex += MeshletMsGroupSize)
    {
        uint vertexIndex = sUniqueSourceVertex[localVertex];
        uint vOff = vertexByteOffset + vertexIndex * 48u;

        float3 localPosition = asfloat(sourceVertices.Load3(vOff + 0u));
        float3 localNormal = asfloat(sourceVertices.Load3(vOff + 12u));
        float2 uv = asfloat(sourceVertices.Load2(vOff + 24u));
        float4 localTangent = asfloat(sourceVertices.Load4(vOff + 32u));

        MSVertexOut vertexOut;
        vertexOut.position = mul(clipFromLocal, float4(localPosition, 1.0));
        vertexOut.worldPosition = mul(worldFromLocal, float4(localPosition, 1.0)).xyz;
        vertexOut.normal = mul(normalFromLocal, float4(localNormal, 0.0)).xyz;
        vertexOut.uv = uv;
        if ((item.flags & 1u) != 0u)
        {
            vertexOut.tangent = localTangent;
        }
        else
        {
            vertexOut.tangent = float4(mul(worldFromLocal, float4(localTangent.xyz, 0.0)).xyz, localTangent.w);
        }
        outVertices[localVertex] = vertexOut;
    }

    if (gtid < triCount)
    {
        outTriangles[gtid] = uint3(
            LookupUniqueVertex(srcTri.x),
            LookupUniqueVertex(srcTri.y),
            LookupUniqueVertex(srcTri.z));
    }
}
