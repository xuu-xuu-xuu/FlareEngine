#ifndef FLARE_ENGINE_MATERIAL_COVERAGE
#define FLARE_ENGINE_MATERIAL_COVERAGE
cbuffer MaterialSurfaceConstants:register(b4)
{
    float4 surfaceBaseColorFactor;
    float4 surfaceCoverage; // cutoff、MaterialAlphaMode、doubleSided、world determinant sign。
#ifdef SE_USER_SURFACE
    float4 flareTime;       // x=time, y=sin(time), z=cos(time), w=deltaTime
    float4 flareUserParams[8];
#endif
};
// 主颜色、阴影和通用预绘只使用这一份覆盖实现。cutoff>1 自然全裁，cutoff=0 保留 alpha=0。
float MaterialCoverage(float2 uv,bool logicalFront)
{
    if(!logicalFront&&surfaceCoverage.z<.5)discard;
    if(logicalFront&&surfaceCoverage.z>1.5)discard;
    float alpha=baseColorMap.Sample(materialSampler,uv).a*surfaceBaseColorFactor.a;
    if(surfaceCoverage.y>.5&&surfaceCoverage.y<1.5)clip(alpha-surfaceCoverage.x);
    return alpha;
}
float MaterialOutputAlpha(float alpha)
{return surfaceCoverage.y>1.5?alpha:1;}
#endif
