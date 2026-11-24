TextureCube<float4> irradianceMap:register(t4);
TextureCube<float4> specularMap:register(t5);
Texture2D<float4> brdfMap:register(t6);
SamplerState environmentSampler:register(s2);
float3 EnvironmentDirection(float3 direction)
{
    return float3(environmentParameters.y*direction.x-environmentParameters.z*direction.z,direction.y,
        environmentParameters.z*direction.x+environmentParameters.y*direction.z);
}
float3 EnvironmentBrdfEx(float3 color,float metal,float rough,float3 n,float3 v
#ifdef SE_SHADING_EXTENSIONS
    ,float3 position,float3 geometricNormal,float3 tangent,float bitangentSign,float faceSign,float2 materialUv
#endif
    ,float materialAo,float dielectricSpec)
{
    float nv=saturate(dot(n,v));
    float dielectricF0=abs(dielectricSpec-0.5)>1e-5?(0.08*saturate(dielectricSpec)):0.04;
    float3 f0=lerp(dielectricF0.xxx,color,metal);
    float specAo=materialAo<0.9999?saturate(pow(abs(nv+materialAo),exp2(-16.0*rough-1.0))-1.0+materialAo):1.0;
    float3 fresnel=f0+(1-f0)*pow(1-nv,5);
    float3 diffuse=irradianceMap.SampleLevel(environmentSampler,EnvironmentDirection(n),0).rgb*color*(1-metal)*(1-fresnel)/3.141592653589793;
#ifdef SE_SHADING_EXTENSIONS
    float3 environmentDiffuseRadiance=irradianceMap.SampleLevel(environmentSampler,EnvironmentDirection(n),0).rgb/3.141592653589793;
    bool usedLocal=false;
    float3 diffuseRadiance=environmentDiffuseRadiance;
    if(shadingModes.y>.5)diffuseRadiance=DiffuseSh(position,n,EnvironmentDirection(n),usedLocal);
    // 烘焙的局部点光反弹是绝对辐照，不能再次乘实时环境强度。
    float diffuseGain=usedLocal?1:environmentParameters.x;
    diffuse=diffuseRadiance*color*(1-metal)*(1-fresnel);
#endif
    float3 reflected=reflect(-v,n);
    float3 radiance=specularMap.SampleLevel(environmentSampler,EnvironmentDirection(reflected),rough*environmentParameters.w).rgb;
    float2 integrated=brdfMap.SampleLevel(environmentSampler,float2(nv,rough),0).rg;
    float3 result=(diffuse*materialAo+radiance*(f0*integrated.x+integrated.y)*specAo)*environmentParameters.x;
#ifdef SE_SHADING_EXTENSIONS
    result=diffuse*diffuseGain*materialAo+radiance*(f0*integrated.x+integrated.y)*environmentParameters.x*specAo;
#endif
    if(skin.z>0.5)
    {
        // Fdez-Aguera 2019，JCGT 8(1)，含 1/pi 勘误：
        // https://jcgt.org/published/0008/01/03/paper.pdf
        // LUT 的 A+B 是单散射方向反照率；有限样本可略超 1，只在新路径按比例归一化。
        float2 energyIntegrated=integrated/max(1.0,integrated.x+integrated.y);
        float singleEnergy=saturate(energyIntegrated.x+energyIntegrated.y);
        float3 singleScatter=f0*energyIntegrated.x+energyIntegrated.y;
        float missingEnergy=1.0-singleEnergy;
        float3 averageFresnel=f0+(1.0-f0)/21.0;
        float3 multipleScatter=singleScatter*missingEnergy*averageFresnel/max(1.0-missingEnergy*averageFresnel,1e-4);
        float3 diffuseWeight=saturate(1.0-singleScatter-multipleScatter)*(1.0-metal)*color;
        // 多次散射近似为宽瓣，以 irradiance/pi 照明，不能直接乘窄镜面预滤波 radiance。
        // 余量留给介质漫反射：白金属/白介质在单位白环境均为 1，不凭空增亮。
#ifndef SE_SHADING_EXTENSIONS
        float3 diffuseRadiance=irradianceMap.SampleLevel(environmentSampler,EnvironmentDirection(n),0).rgb/3.141592653589793;
#endif
#ifdef SE_SHADING_EXTENSIONS
        // SH只替换漫反射；窄瓣镜面和多散射镜面仍使用同一原环境参考。
        result=(radiance*singleScatter*specAo+environmentDiffuseRadiance*multipleScatter*materialAo)*environmentParameters.x
            +diffuseRadiance*diffuseWeight*diffuseGain*materialAo;
#else
        result=(radiance*singleScatter*specAo+diffuseRadiance*(multipleScatter+diffuseWeight)*materialAo)*environmentParameters.x;
#endif
    }
#ifdef SE_SHADING_EXTENSIONS
    float3 sheen=saturate(layerSheen.rgb);float sr=max(.07,layerSheen.w);
    if(LayerTexture(3))sheen*=sheenColorMap.Sample(materialSampler,materialUv).rgb;
    if(LayerTexture(4))sr=max(.07,sr*sheenRoughnessMap.Sample(materialSampler,materialUv).a);
    if(probeOrigin.w>.5&&any(sheen>0))
    {
        float energy=ShadingLinear(ltcAmplitudeMap,LtcUv(max(nv,.0001),sr)).b;
        result*=saturate(1-max(sheen.r,max(sheen.g,sheen.b))*energy);
        // 首版使用宽瓣环境近似；不把GGX预滤波贴图声称为Charlie卷积。
        result+=environmentDiffuseRadiance*sheen*energy*environmentParameters.x;
    }
    float cw=saturate(layerCoat.x),cr=max(.045,layerCoat.y);
    if(LayerTexture(0))cw*=coatWeightMap.Sample(materialSampler,materialUv).r;
    if(LayerTexture(1))cr=max(.045,cr*coatRoughnessMap.Sample(materialSampler,materialUv).g);
    if(cw>0)
    {
        float3 cn=geometricNormal*faceSign;
        if(LayerTexture(2))
        {float3 ct=LayerTangent(cn,tangent),cb=cross(cn,ct)*bitangentSign,mapped=coatNormalMap.Sample(materialSampler,materialUv).xyz*2-1;mapped.xy*=layerCoat.z;cn=UnitVector(ct*mapped.x+cb*mapped.y+cn*mapped.z,cn);}
        cn*=faceSign;
        float cv=saturate(dot(cn,v)),F=.04+.96*pow(1-cv,5);
        float2 ab=brdfMap.SampleLevel(environmentSampler,float2(cv,cr),0).rg;
        float3 coatRadiance=specularMap.SampleLevel(environmentSampler,EnvironmentDirection(reflect(-v,cn)),cr*environmentParameters.w).rgb;
        result*=1-cw*F;result+=cw*coatRadiance*(.04*ab.x+ab.y)*environmentParameters.x;
    }
#endif
    return result;
}
float3 EnvironmentBrdf(float3 color,float metal,float rough,float3 n,float3 v
#ifdef SE_SHADING_EXTENSIONS
    ,float3 position,float3 geometricNormal,float3 tangent,float bitangentSign,float faceSign,float2 materialUv
#endif
)
{
    return EnvironmentBrdfEx(color,metal,rough,n,v
#ifdef SE_SHADING_EXTENSIONS
        ,position,geometricNormal,tangent,bitangentSign,faceSign,materialUv
#endif
        ,1.0,0.5);
}
