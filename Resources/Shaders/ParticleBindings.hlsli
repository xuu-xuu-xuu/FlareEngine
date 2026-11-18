#ifndef SE_PARTICLE_BINDINGS_HLSLI
#define SE_PARTICLE_BINDINGS_HLSLI

cbuffer ParticleFrameConstants : register(b0)
{
    column_major float4x4 particleClipFromWorld;
    float4 particleCameraRight;
    float4 particleCameraUp;
    float4 particleViewport;
};

struct ParticleInstanceGpuData
{
    float4 positionAndSize;
    float4 color;
    float4 params;
};

StructuredBuffer<ParticleInstanceGpuData> particleInstances : register(t0);

#endif
