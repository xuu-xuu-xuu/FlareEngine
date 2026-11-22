#ifndef FLARE_ENGINE_AMBIENT_OCCLUSION_BINDINGS
#define FLARE_ENGINE_AMBIENT_OCCLUSION_BINDINGS
// 私有 compute b0，272B；图形端沿 schema40 的 b5/b6，不与这里混用。
cbuffer AmbientOcclusionConstants:register(b0)
{
    column_major float4x4 aoClipFromView,aoViewFromClip,aoViewFromWorld;
    float4 aoViewport; // fullWidth/fullHeight/ceil(width/2)/ceil(height/2)
    float4 aoParameters; // radius/intensity/bias/0，均为世界/视空间米制。
    float4 contactParameters; // length/bias/thickness/fadeFraction
    float4 aoLightDirection; // 视空间指向方向光；w 表示方向光实际开启。
    float4 aoModes; // GTAO/contact，二者独立。
};
Texture2D<float> prepassDepth:register(t0);
Texture2D<float2> prepassNormal:register(t1);
Texture2D<float2> halfOcclusion:register(t2);
RWTexture2D<float2> halfOutput:register(u0);
RWTexture2D<float2> visibilityOutput:register(u1);
static const float AoPi=3.14159265358979323846;
float3 AoUnit(float3 value,float3 fallback)
{float squared=dot(value,value);return squared>1e-16&&isfinite(squared)?value*rsqrt(squared):fallback;}
bool AoInside(int2 pixel)
{return all(pixel>=0)&&all(pixel<int2(aoViewport.xy));}
bool AoUnproject(float2 pixel,float depth,out float3 position)
{
    float2 ndc=(pixel+.5)/aoViewport.xy*float2(2,-2)+float2(-1,1);
    float4 projected=mul(aoViewFromClip,float4(ndc,depth,1));
    bool valid=all(isfinite(projected))&&abs(projected.w)>1e-8;
    position=valid?projected.xyz/projected.w:float3(0,0,0);valid=valid&&all(isfinite(position));return valid;
}
bool AoReadPosition(int2 pixel,out float3 position)
{
    position=0;bool valid=AoInside(pixel);
    if(valid)
    {
        float depth=prepassDepth.Load(int3(pixel,0));valid=isfinite(depth)&&depth>=0&&depth<1;
        if(valid)valid=AoUnproject(float2(pixel),depth,position)&&position.z<-.00001;
    }
    return valid;
}
bool AoReadSurface(int2 pixel,out float3 position,out float3 normal)
{
    normal=float3(0,0,1);bool valid=AoReadPosition(pixel,position);
    if(valid)
    {
        float2 encoded=prepassNormal.Load(int3(pixel,0));valid=all(isfinite(encoded))&&all(abs(encoded)<=1.0001);
        if(valid)
        {
            float3 worldNormal=float3(encoded,1-abs(encoded.x)-abs(encoded.y));
            if(worldNormal.z<0)worldNormal.xy=(1-abs(worldNormal.yx))*float2(worldNormal.x<0?-1:1,worldNormal.y<0?-1:1);
            normal=AoUnit(mul(aoViewFromWorld,float4(AoUnit(worldNormal,float3(0,0,1)),0)).xyz,float3(0,0,1));
        }
    }
    return valid;
}
bool AoProject(float3 position,out int2 pixel)
{
    pixel=0;float4 clip=mul(aoClipFromView,float4(position,1));
    bool valid=position.z<-.00001&&all(isfinite(clip))&&clip.w>1e-8;
    if(valid)
    {
        float3 ndc=clip.xyz/clip.w;float2 uv=ndc.xy*float2(.5,-.5)+.5;
        valid=all(uv>=0)&&all(uv<1)&&ndc.z>=0&&ndc.z<=1;
        if(valid)pixel=(int2)floor(uv*aoViewport.xy);
    }
    return valid;
}
#endif
