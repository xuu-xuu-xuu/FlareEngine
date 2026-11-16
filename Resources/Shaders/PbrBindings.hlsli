#ifndef SELF_ENGINE_PBR_BINDINGS
#define SELF_ENGINE_PBR_BINDINGS// 布局 2：与 MaterialLayout.h 的 PbrDrawConstants 精确对应。
cbuffer PbrConstants : register(b0)
{
    column_major float4x4 clipFromLocal;
    column_major float4x4 worldFromLocal;
    column_major float4x4 normalFromLocal;
    float4 baseColor;
    float4 cameraPosition;
    float4 directionToLight;
    float4 radiance;
    float4 parameters; // metallic、roughness、normalScale、世界线性行列式符号。
    float4 options; // normalMapping、unlit、doubleSided；w 为 receivesShadow，G05.08 起非整数时表示 LOD dither。
    float4 look; // W06：x 模式（0 基础色/1 白模/2 Toon），y 混合，z Toon 分档，w 边缘强度。
    float4 skin; // xy：W03 调色板字节偏移/关节数；zw：IBL 多散射/法线镜面 AA 开关，旧绘制为 0。
};
Texture2D<float4> baseColorMap : register(t0);
Texture2D<float4> normalMap : register(t1);
Texture2D<float4> metallicRoughnessMap : register(t2);
SamplerState materialSampler : register(s0);
float3 UnitVector(float3 value,float3 fallback)
{
    float squared=dot(value,value);
    // 显式有限范围也能处理 NaN/Inf，避免 FXC 在常量向量调用处将 isfinite 诊断升级为错误。
    return squared>1e-12&&squared<=3.402823466e38?value*rsqrt(squared):fallback;
}
// G05.08：与 CPU LodSelect::Bayer4x4Threshold 完全一致的 4×4 Bayer 阈值（(index+0.5)/16）。
float LodBayerThreshold(uint2 pixel)
{
    const float bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    return (bayer[(pixel.y&3)*4+(pixel.x&3)]+0.5)/16.0;
}
float3 OutputSrgb(float3 color)
{
    float3 value=saturate(color);
    return lerp(value*12.92,1.055*pow(value,1.0/2.4)-0.055,step(0.0031308,value));
}
// 屏幕空间法线方差近似：将过滤核并入 GGX 的 alpha^2（感知粗糙度的四次方）。
// 系数 0.15、核阈值 0.2^2 限制边界导数误差；不是完整 Toksvig/LEAN 资产管线。
// point 法线采样在 <=1 texel/px 已被分辨，不扩大近景高光；1..2 texel/px 连续过渡。
float FilterSpecularRoughness(float rough,float3 normalDx,float3 normalDy,float footprint)
{
    float variance=0.15*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    float kernel=min(2.0*variance,0.04)*smoothstep(1.0,2.0,footprint);
    float alpha=rough*rough;
    return sqrt(sqrt(saturate(alpha*alpha+kernel)));
}
float3 DirectBrdf(float3 color,float metal,float rough,float3 n,float3 v,float3 l)
{
    const float pi=3.14159265358979323846;
    float nl=saturate(dot(n,l));
    float nv=max(saturate(dot(n,v)),1e-4);
    float3 h=UnitVector(v+l,n);float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
    float alpha=rough*rough,a2=alpha*alpha;
    float divisor=nh*nh*(a2-1)+1;
    float distribution=a2/max(pi*divisor*divisor,1e-12);
    float visibility=0.5/max(nl*sqrt(nv*nv*(1-a2)+a2)+nv*sqrt(nl*nl*(1-a2)+a2),1e-8);
    float3 f0=lerp(float3(0.04,0.04,0.04),color,metal);
    float grazing=1-vh;float grazing5=grazing*grazing*grazing*grazing*grazing;
    float3 fresnel=f0+(1-f0)*grazing5;
    float3 specular=distribution*visibility*fresnel;
    float3 diffuse=(1-fresnel)*(1-metal)*color/pi;
    return (diffuse+specular)*nl;
}
// W06：外观覆盖只读合成。mode 0 原样返回，1 为白模（忽略固有色与纹理），2 为三档 Toon。
// ndl 是方向光几何项，visibility 是阴影可见度；都不回写材质或资产。
float3 ApplyLook(float3 lit,float3 albedo,float3 n,float3 v,float ndl,float visibility)
{
    float mode=look.x;
    float blend=saturate(look.y);
    float3 result=lit;
    if(mode>=0.5&&blend>0.001)
    {
        float shaped=saturate(ndl)*saturate(visibility);
        float3 clay=float3(0.80,0.80,0.80)*(0.20+0.80*shaped);
        float level=shaped>=0.55?1.0:(shaped>=0.16?0.55:0.0);
        float3 toonShadow=albedo*float3(0.58,0.56,0.66);
        float3 toon=lerp(toonShadow,albedo,level);
        float rim=pow(1.0-saturate(dot(n,v)),3.0)*saturate(look.w)*saturate(visibility);
        toon+=float3(1.0,0.92,0.84)*rim*0.16;
        float3 chosen=clay;
        if(mode>=1.5)chosen=toon;
        result=lerp(lit,chosen,blend);
    }
    return result;
}
#endif
