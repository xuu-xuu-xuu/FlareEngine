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
        float2 uvTex=uv*size;
        int2 center=clamp((int2)floor(uvTex),int2(0,0),int2(size-1,size-1));
        float2 s=clamp(uvTex-(float2(center)+0.5),float2(-0.5,-0.5),float2(0.5,0.5));
        // 二次 B 样条亚纹素权重（3×3 共 9 次 Load，C1 连续消除聚光/点光阶梯锯齿，精确归一权重和恒为 1）。
        float wx0=0.5*(0.5-s.x)*(0.5-s.x),wx2=0.5*(0.5+s.x)*(0.5+s.x);
        float wy0=0.5*(0.5-s.y)*(0.5-s.y),wy2=0.5*(0.5+s.y)*(0.5+s.y);
        float wx[3]={wx0,1.0-wx0-wx2,wx2};
        float wy[3]={wy0,1.0-wy0-wy2,wy2};
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
#ifdef SE_SHADOW_BINDINGS
        // 直接表面光照：硬件 2×2 双线性 SampleCmpLevelZero + 12-Tap 黄金角 Vogel 旋转圆盘，柔和消除点光/聚光阴影像素阶梯。
        float halfTexel=0.5/max((float)size,1.0);
        float2 uvMin=float2(halfTexel,halfTexel),uvMax=float2(1.0-halfTexel,1.0-halfTexel);
        float diskRadius=1.65/max((float)size,1.0);
        float hashPhase=frac(52.9829189*frac(dot(world.xz+world.yy*0.37,float2(12.9898,78.233))))*6.2831853;
        float sRot,cRot;
        sincos(hashPhase,sRot,cRot);
        float2 clampGrad=clamp(gradient,float2(-0.08,-0.08),float2(0.08,0.08));
        [unroll]for(uint k=0;k<12u;++k)
        {
            float r=sqrt(((float)k+0.5)/12.0)*diskRadius;
            float theta=(float)k*2.3999632;
            float st,ct;
            sincos(theta,st,ct);
            float2 dUv=float2(ct*cRot-st*sRot,ct*sRot+st*cRot)*r;
            float2 sampleUv=clamp(uv+dUv,uvMin,uvMax);
            float reference=ndc.z+dot(clampGrad,sampleUv-uv)-4e-6;
            sum+=localShadowMap.SampleCmpLevelZero(shadowSampler,float3(sampleUv,(float)slice),reference);
        }
        visibility=sum*(1.0/12.0);
#else
        [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
        {
            int2 texel=clamp(center+int2(x,y),int2(0,0),int2(size-1,size-1));
            float2 texelCenter=(float2(texel)+.5)/size;
            float reference=ndc.z+dot(gradient,texelCenter-uv)-2e-6;
            float tap=(reference<=localShadowMap.Load(int4(texel,(int)slice,0)))?1.0:0.0;
            sum+=tap*wx[x+1]*wy[y+1];
        }
        visibility=sum;
#endif
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
