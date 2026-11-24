#ifndef FLARE_USER_SKY_CONTRACT_HLSLI
#define FLARE_USER_SKY_CONTRACT_HLSLI

struct FlareSkyInput
{
    float3 viewDir;
    float2 screenUv;
    float2 pixelPos;
    float3 sunDir;
    float  sunIntensity;
    float3 sunColor;
    float3 zenithColor;
    float3 horizonColor;
    float3 groundColor;
    float  cloudCoverage;
    float  cloudDensity;
    float  cloudSpeed;
    float  time;
};

#include "FlareUserSkyBody.hlsli"

#endif
