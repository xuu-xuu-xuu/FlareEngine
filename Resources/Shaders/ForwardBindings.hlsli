cbuffer ForwardConstants:register(b1)
{
    float4 frameDirection;
    float4 frameRadiance;
    float4 frameSkyExposure;
    float4 framePointCount;
    float4 pointPositionRadius[8];
    float4 pointRadiance[8];
#if defined(SE_CASCADED) || defined(SE_CLUSTERED)
    column_major float4x4 csmClipFromWorld[4];
    float4 csmSplits;
    float4 csmViewDepth;
    float4 shadowParameters;
    float4 csmOptions;
    float4 environmentParameters;
#else
#ifdef SE_SHADOWS
    column_major float4x4 shadowClipFromWorld;
    float4 shadowParameters;
#endif
#endif
#if defined(SE_ENVIRONMENT) && !defined(SE_CASCADED) && !defined(SE_CLUSTERED)
    float4 environmentParameters;
#endif
#ifdef SE_CLUSTERED
    column_major float4x4 clusterView;
    column_major float4x4 clusterProjection;
    column_major float4x4 clusterInverseProjection;
    float4 clusterParameters;
    float4 clusterViewport;
    float4 clusterModes;
#endif
};
#if defined(SE_CLUSTERED) && !defined(SE_CASCADED)
#define shadowClipFromWorld csmClipFromWorld[0]
#endif
