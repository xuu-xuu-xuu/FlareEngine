#include "ParticleBindings.hlsli"

struct ParticleVertexOutput
{
    float4 clipPosition : SV_POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float softFactor : TEXCOORD1;
};

ParticleVertexOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1.0f, -1.0f),
        float2( 1.0f, -1.0f),
        float2( 1.0f,  1.0f),
        float2(-1.0f, -1.0f),
        float2( 1.0f,  1.0f),
        float2(-1.0f,  1.0f)
    };

    const ParticleInstanceGpuData instanceData = particleInstances[instanceId];
    const float2 corner = corners[vertexId % 6u];
    const float angle = instanceData.params.x;
    const float sinA = sin(angle);
    const float cosA = cos(angle);
    const float2 rotated = float2(
        corner.x * cosA - corner.y * sinA,
        corner.x * sinA + corner.y * cosA);

    const float halfSize = max(instanceData.positionAndSize.w, 0.0f) * 0.5f;
    const float3 worldPosition = instanceData.positionAndSize.xyz
        + particleCameraRight.xyz * (rotated.x * halfSize)
        + particleCameraUp.xyz * (rotated.y * halfSize);

    ParticleVertexOutput output;
    output.clipPosition = mul(particleClipFromWorld, float4(worldPosition, 1.0f));
    output.color = instanceData.color;
    output.uv = corner;
    output.softFactor = max(instanceData.params.z, 0.5f);
    return output;
}

float4 PSMain(ParticleVertexOutput input) : SV_TARGET
{
    const float radiusSq = dot(input.uv, input.uv);
    clip(1.0f - radiusSq);

    const float radial = pow(saturate(1.0f - radiusSq), input.softFactor);
    const float alpha = saturate(input.color.a * radial);
    clip(alpha - (1.0f / 1024.0f));

    return float4(max(input.color.rgb, 0.0f) * alpha, alpha);
}
