#ifndef SE_LOCAL_LIGHT_BINDINGS
#define SE_LOCAL_LIGHT_BINDINGS
cbuffer LocalLightConstants : register(b7)
{
    float4 localLightInfo;
    float4 localPositionRange[8],localRadianceSpot[8],localDirectionOuter[8],localConeShadow[8];
    column_major float4x4 localClipFromWorld[48];
};
Texture2DArray<float> localShadowMap : register(t25);

uint LocalPointFace(float3 outward)
{
    float3 axis=abs(outward);
    uint face=outward.z>=0?4u:5u;
    if(axis.x>=axis.y&&axis.x>=axis.z)face=outward.x>=0?0u:1u;
    else if(axis.y>=axis.z)face=outward.y>=0?2u:3u;
    return face;
}

float LocalLightVisibility(uint i,float3 world,float3 normal,float3 toLight,float distanceToLight)
{
    // 次 texel 的偏移只处理量化/曲面误差；各 PCF texel 的投影深度由接收面解析给出。
    float footprint=distanceToLight/max(localLightInfo.y,1.0);
    float3 facingNormal=dot(normal,toLight)<0?-normal:normal;
    float3 receiver=world+footprint*(facingNormal*.12+toLight*.08);
    uint slice=(uint)localConeShadow[i].y;
    if(localRadianceSpot[i].w<.5)slice+=LocalPointFace(receiver-localPositionRange[i].xyz);
    float4 clip=mul(localClipFromWorld[min(slice,47u)],float4(receiver,1));
    float visibility=1;
    float3 ndc=clip.xyz/max(clip.w,1e-8);
    if(clip.w>0&&ndc.z>0&&ndc.z<1&&all(abs(ndc.xy)<=1.00001))
    {
        int size=(int)localLightInfo.y;
        float2 uv=ndc.xy*float2(.5,-.5)+.5;
        int2 center=clamp((int2)(uv*size),int2(0,0),int2(size-1,size-1));
        // 对透视投影 z=A+B/viewDepth。平面与像素射线的交点满足
        // 1/viewDepth=dot(n,ray)/dot(n,receiver-light)。因此 z 对纹理 UV 是仿射函数。
        // 索引采用实际面矩阵，聚光锥角及点灯上下两面同样成立。雾体样点没有切面，梯度为零。
        float planeDistance=dot(facingNormal,receiver-localPositionRange[i].xyz);
        float2 gradient=0;
        if(abs(planeDistance)>1e-5*max(distanceToLight,.01))
        {
            float3 rowX=localClipFromWorld[slice][0].xyz,rowY=localClipFromWorld[slice][1].xyz;
            float gradientScale=2*localConeShadow[i].z*sign(planeDistance)/max(abs(planeDistance),1e-8);
            gradient=gradientScale*float2(dot(facingNormal,rowX)/max(dot(rowX,rowX),1e-8),
                -dot(facingNormal,rowY)/max(dot(rowY,rowY),1e-8));
        }
        float sum=0;
        [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
        {
            int2 texel=clamp(center+int2(x,y),int2(0,0),int2(size-1,size-1));
            float2 texelCenter=(float2(texel)+.5)/size;
            float reference=ndc.z+dot(gradient,texelCenter-uv)-2e-6;
            sum+=(reference<=localShadowMap.Load(int4(texel,(int)slice,0)))?1.0:0.0;
        }
        visibility=sum/9.0;
    }
    return visibility;
}

float LocalLightAttenuation(uint i,float3 world,float3 normal,bool receiveShadow,out float3 toLight)
{
    float3 delta=localPositionRange[i].xyz-world;
    float squared=dot(delta,delta),radius=localPositionRange[i].w;
    float distanceToLight=sqrt(max(squared,1e-12));
    toLight=squared>1e-12?delta/distanceToLight:float3(0,1,0);
    float ratio=squared/(radius*radius),window=saturate(1-ratio*ratio);
    float attenuation=window*window/max(squared,.0025);
    if(localRadianceSpot[i].w>.5)
    {
        float angle=dot(-toLight,localDirectionOuter[i].xyz),outer=localDirectionOuter[i].w,inner=localConeShadow[i].x;
        float cone=saturate((angle-outer)/max(inner-outer,1e-6));
        attenuation*=inner>outer?cone*cone*(3-2*cone):step(outer,angle);
    }
    if(attenuation>0&&receiveShadow&&localConeShadow[i].y>=0)
        attenuation*=LocalLightVisibility(i,world,normal,toLight,distanceToLight);
    return attenuation;
}
#endif
