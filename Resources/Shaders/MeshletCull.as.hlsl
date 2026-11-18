#include "MeshletBindings.hlsli"

cbuffer MeshletDispatchConstants : register(b0, space1)
{
    uint firstMeshlet;
    uint dispatchMeshletCount;
    uint vertexByteOffset;
    uint indexElementOffset;
};

cbuffer MeshletCullConstants : register(b1, space1)
{
    float4 planes[6];
    uint totalMeshlets;
    uint3 reserved;
    column_major float4x4 clipFromWorld;
    float4 screen; // width, height, depthBias, mipCount
    uint occlusionEnabled;
    uint3 occlusionReserved;
};

#include "VisibilityPredicate.hlsli"

StructuredBuffer<VisibilityItem> meshletBounds : register(t0, space1);
Texture2D<float> currentFrameHiZ : register(t4, space1);

bool HiddenByCurrentFrame(VisibilityItem item)
{
    bool hidden = false;
    if (occlusionEnabled != 0)
    {
        float2 minimum = 1e30;
        float2 maximum = -1e30;
        float nearest = 1;
        bool valid = true;
        [unroll] for (uint cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
        {
            float3 position = float3(
                (cornerIndex & 1) ? item.maximum.x : item.minimum.x,
                (cornerIndex & 2) ? item.maximum.y : item.minimum.y,
                (cornerIndex & 4) ? item.maximum.z : item.minimum.z);
            float4 clip = mul(clipFromWorld, float4(position, 1));
            float3 ndc = 0;
            bool cornerValid = all(isfinite(clip)) && clip.w > 1e-6;
            if (cornerValid)
            {
                ndc = clip.xyz / clip.w;
                cornerValid = all(isfinite(ndc)) && ndc.z >= 0 && ndc.z <= 1;
            }
            valid = valid && cornerValid;
            if (cornerValid)
            {
                minimum = min(minimum, ndc.xy);
                maximum = max(maximum, ndc.xy);
                nearest = min(nearest, ndc.z);
            }
        }
        valid = valid && all(minimum >= -1) && all(maximum <= 1);
        if (valid)
        {
            float2 lo = float2(minimum.x * 0.5 + 0.5, 0.5 - maximum.y * 0.5) * screen.xy;
            float2 hi = float2(maximum.x * 0.5 + 0.5, 0.5 - minimum.y * 0.5) * screen.xy;
            lo = max(0, lo - 0.01);
            hi = min(screen.xy, hi + 0.01);
            if (all(hi > lo))
            {
                uint level = 0;
                float scale = 1;
                uint levels = (uint)screen.w;
                [loop] while (level + 1 < levels && any((hi - lo) / scale > 1.99))
                {
                    ++level;
                    scale *= 2;
                }
                uint2 dimensions = max(uint2(1, 1), (uint2)screen.xy >> level);
                if (all(hi <= float2(dimensions) * scale + 0.0001))
                {
                    uint2 first = min((uint2)floor(lo / scale), dimensions - 1);
                    uint2 last = min((uint2)max(0, ceil(hi / scale) - 1), dimensions - 1);
                    float farthest = 0;
                    [unroll] for (uint sampleY = 0; sampleY < 4; ++sampleY)
                    {
                        [unroll] for (uint sampleX = 0; sampleX < 4; ++sampleX)
                        {
                            uint2 texel = first + uint2(sampleX, sampleY);
                            if (all(texel <= last))
                                farthest = max(farthest, currentFrameHiZ.Load(int3(texel, level)));
                        }
                    }
                    hidden = isfinite(farthest) && nearest > farthest + screen.z;
                }
            }
        }
    }
    return hidden;
}

groupshared uint s_VisibleMask;
groupshared MeshletPayload s_Payload;

[numthreads(MeshletAsGroupSize, 1, 1)]
void ASMain(uint gtid : SV_GroupThreadID, uint gid : SV_GroupID)
{
    if (gtid == 0)
    {
        s_VisibleMask = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint localOffset = gid * MeshletAsGroupSize + gtid;
    uint globalMeshlet = firstMeshlet + localOffset;
    bool visible = false;
    if (localOffset < dispatchMeshletCount && globalMeshlet < totalMeshlets)
    {
        VisibilityItem item = meshletBounds[globalMeshlet];
        visible = Visible(item) && !HiddenByCurrentFrame(item);
        if (visible)
        {
            InterlockedOr(s_VisibleMask, 1u << gtid);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    uint mask = s_VisibleMask;
    if (visible)
    {
        uint prefixMask = gtid == 0 ? 0u : ((1u << gtid) - 1u);
        uint dstIndex = countbits(mask & prefixMask);
        s_Payload.meshletIndices[dstIndex] = globalMeshlet;
    }
    GroupMemoryBarrierWithGroupSync();

    uint visibleCount = countbits(mask);
    DispatchMesh(visibleCount, 1, 1, s_Payload);
}
