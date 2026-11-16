#ifndef SELF_ENGINE_SHADING_BINDINGS
#define SELF_ENGINE_SHADING_BINDINGS
// schema 40：与 MaterialLayout.h 唯一常量定义一致；旧程序不包含此文件。
cbuffer ShadingFrameConstants : register(b5)
{
    float4 shadingModes;
    float4 shadingViewport;
    float4 probeOrigin;
    float4 probeSpacing;
    float4 probeDimensions;
    float4 globalSh[9];
    float4 rectCenter[4],rectHalfU[4],rectHalfV[4],rectRadiance[4];
};
cbuffer ShadingMaterialConstants : register(b6)
{
    float4 layerCoat,layerSheen,layerAnisotropy,layerSubsurface,layerSubsurfaceColor,layerTextureFlags;
};
Texture2D<float2> screenOcclusionMap : register(t13);
Texture2D<float4> directEnergyMap : register(t14);
Texture2D<float4> diffuseProbeMap : register(t15);
Texture2D<float4> ltcMatrixMap : register(t16);
Texture2D<float4> ltcAmplitudeMap : register(t17);
Texture2D<float4> skinDiffuseMap : register(t18);
Texture2D<float4> coatWeightMap : register(t19);
Texture2D<float4> coatRoughnessMap : register(t20);
Texture2D<float4> coatNormalMap : register(t21);
Texture2D<float4> sheenColorMap : register(t22);
Texture2D<float4> sheenRoughnessMap : register(t23);
Texture2D<float4> anisotropyMap : register(t24);
// 扩展资源共享已有 s0 的采样状态；LUT 手动双线性，不能沿用 wrap/point。
float4 ShadingLinear(Texture2D<float4> image,float2 uv)
{
    uint w=1,h=1;image.GetDimensions(w,h);float2 p=saturate(uv)*float2(w-1,h-1);
    int2 a=(int2)floor(p),b=min(a+1,int2(w-1,h-1));float2 f=frac(p);
    return lerp(lerp(image.Load(int3(a,0)),image.Load(int3(b.x,a.y,0)),f.x),
        lerp(image.Load(int3(a.x,b.y,0)),image.Load(int3(b,0)),f.x),f.y);
}
bool LayerTexture(uint bit){return (((uint)layerTextureFlags.x)&(1u<<bit))!=0;}
float4 ShadingMaterialSample(Texture2D<float4> image,float2 uv,float2 uvDx,float2 uvDy)
{return image.SampleGrad(materialSampler,uv,uvDx,uvDy);}
float2 ScreenOcclusion(float2 pixel,bool transparent)
{
    uint w=1,h=1;screenOcclusionMap.GetDimensions(w,h);
    int2 p=clamp((int2)pixel,int2(0,0),int2(w-1,h-1));
    float2 value=saturate(screenOcclusionMap.Load(int3(p,0)));
    return transparent?float2(1,1):lerp(float2(1,1),value,step(.5,shadingViewport.zw));
}
float3 DiffuseSh(float3 p,float3 n,float3 globalNormal,out bool usedLocal)
{
    float3 coefficients[9];[unroll]for(uint globalIndex=0;globalIndex<9;++globalIndex)coefficients[globalIndex]=globalSh[globalIndex].xyz;
    bool local=false;
    if(shadingModes.z>.5)
    {
        float3 q=(p-probeOrigin.xyz)/max(probeSpacing.xyz,1e-6);
        int3 dimensions=(int3)probeDimensions.xyz,c=min((int3)floor(q),dimensions-2);
        if(all(q>=0)&&all(q<=dimensions-1)&&all(dimensions>=2))
        {
            uint cell=(uint)(c.x+c.y*dimensions.x+c.z*dimensions.x*dimensions.y);
            bool valid=diffuseProbeMap.Load(int3(9,cell,0)).y>.5;
            [unroll]for(uint validCorner=0;validCorner<8;++validCorner)
            {
                int3 corner=c+int3(validCorner&1,(validCorner>>1)&1,(validCorner>>2)&1);
                uint row=(uint)(corner.x+corner.y*dimensions.x+corner.z*dimensions.x*dimensions.y);
                valid=valid&&diffuseProbeMap.Load(int3(9,row,0)).x>.5;
            }
            if(valid)
            {
                local=true;float3 f=q-c;
                [unroll]for(uint zeroIndex=0;zeroIndex<9;++zeroIndex)coefficients[zeroIndex]=0;
                [unroll]for(uint blendCorner=0;blendCorner<8;++blendCorner)
                {
                    int3 corner=c+int3(blendCorner&1,(blendCorner>>1)&1,(blendCorner>>2)&1);
                    uint row=(uint)(corner.x+corner.y*dimensions.x+corner.z*dimensions.x*dimensions.y);
                    float3 w=lerp(1-f,f,float3(blendCorner&1,(blendCorner>>1)&1,(blendCorner>>2)&1));
                    [unroll]for(uint coefficientIndex=0;coefficientIndex<9;++coefficientIndex)coefficients[coefficientIndex]+=diffuseProbeMap.Load(int3(coefficientIndex,row,0)).xyz*(w.x*w.y*w.z);
                }
            }
        }
    }
    float3 d=local?n:globalNormal;
    float basis[9]={.2820947918,.4886025119*d.y,.4886025119*d.z,.4886025119*d.x,
        1.0925484306*d.x*d.y,1.0925484306*d.y*d.z,.3153915653*(3*d.z*d.z-1),
        1.0925484306*d.x*d.z,.5462742153*(d.x*d.x-d.y*d.y)};
    float3 result=0;[unroll]for(uint sumIndex=0;sumIndex<9;++sumIndex)result+=coefficients[sumIndex]*basis[sumIndex];
    usedLocal=local;return max(result,0); // 数据保持 signed；仅最终重建辐照作非负裁剪。
}
float2 EnergyUv(float nv,float rough)
{return float2((sqrt(clamp(nv,.0001,1))-.01)/.99,(rough-.045)/.955);}
float2 LtcUv(float nv,float rough)
{return float2((rough-.045)/.955,2.0/3.14159265359*acos(sqrt(saturate((nv-.0001)/.9999))));}
float3 FresnelSchlick(float3 f0,float cosine)
{return f0+(1-f0)*pow(1-saturate(cosine),5);}
float3 LayerTangent(float3 n,float3 tangent)
{
    float3 fallback=abs(n.z)<.9?cross(float3(0,0,1),n):cross(float3(0,1,0),n);
    return UnitVector(tangent-n*dot(n,tangent),UnitVector(fallback,float3(1,0,0)));
}
float3 SurfaceDirectBrdf(float3 color,float metal,float rough,float3 n,float3 geometricNormal,float3 v,float3 l,
    float3 tangent,float bitangentSign,float faceSign,float2 uv,float2 uvDx,float2 uvDy,float curvature)
{
    const float pi=3.141592653589793;
    float nl=saturate(dot(n,l)),nv=max(saturate(dot(n,v)),.0001);
    float3 h=UnitVector(v+l,n),f0=lerp(.04.xxx,color,metal),F=FresnelSchlick(f0,dot(v,h));
    float3 t=LayerTangent(n,tangent),b=cross(n,t)*bitangentSign*faceSign;
    float strength=saturate(layerAnisotropy.x),angle=layerAnisotropy.y;
    if(LayerTexture(5))
    {
        float3 sampleValue=ShadingMaterialSample(anisotropyMap,uv,uvDx,uvDy).rgb;
        strength*=sampleValue.b;float2 direction=sampleValue.rg*2-1;
        if(dot(direction,direction)>1e-8)angle+=atan2(direction.y,direction.x);
    }
    float ca=cos(angle),sa=sin(angle);float3 at=ca*t+sa*b,ab=-sa*t+ca*b;
    // KHR_materials_anisotropy 的 alphaT/alphaB 映射，不借用另一套Disney参数语义。
    float a=rough*rough,ax=max(lerp(a,1,strength*strength),.002025),ay=max(a,.002025);
    float ht=dot(h,at),hb=dot(h,ab),nh=saturate(dot(n,h));
    float denominator=ht*ht/(ax*ax)+hb*hb/(ay*ay)+nh*nh;
    float distribution=1/max(pi*ax*ay*denominator*denominator,1e-12);
    float lv=sqrt(1+(ax*ax*pow(dot(v,at),2)+ay*ay*pow(dot(v,ab),2))/(nv*nv));
    float ll=sqrt(1+(ax*ax*pow(dot(l,at),2)+ay*ay*pow(dot(l,ab),2))/max(nl*nl,1e-8));
    float3 specular=distribution*F/max(2*nl*nv*(lv+ll),1e-8);
    float3 diffuse=(1-F)*(1-metal)*color/pi;
    // Kulla–Conty：方向能量与余弦平均均来自同一 GGX 积分，禁止拿IBL LUT冒充。
    if(shadingModes.x>.5&&strength<1e-5)
    {
        float4 eo=ShadingLinear(directEnergyMap,EnergyUv(nv,rough));
        float4 ei=ShadingLinear(directEnergyMap,EnergyUv(max(nl,.0001),rough));
        float Eo=saturate(eo.x+eo.y),Ei=saturate(ei.x+ei.y),Eavg=saturate(eo.z+eo.w);
        float3 Favg=f0+(1-f0)/21.0;
        float3 Fms=Favg*Eavg/max(1-Favg*(1-Eavg),1e-5);
        specular+=Fms*(1-Eo)*(1-Ei)/max(pi*(1-Eavg),1e-5);
        float3 Ro=saturate(f0*eo.x+eo.y+Fms*(1-Eo));
        float3 Ri=saturate(f0*ei.x+ei.y+Fms*(1-Ei));
        float3 Ravg=saturate(f0*eo.z+eo.w+Fms*(1-Eavg));
        diffuse=(1-metal)*color*(1-Ro)*(1-Ri)/max(pi*(1-Ravg),1e-5);
    }
    float3 diffuseContribution=diffuse*nl;
    if(layerSubsurface.x>0&&probeDimensions.w>.5)
    {
        float q=saturate(layerSubsurface.y*curvature/8.0);
        float3 profile=ShadingLinear(skinDiffuseMap,float2(dot(n,l)*.5+.5,sqrt(q))).rgb;
        float3 scattered=diffuse*lerp(nl.xxx,profile*layerSubsurfaceColor.rgb,saturate(layerSubsurface.x));
        diffuseContribution=scattered;
    }
    float3 result=specular*nl+diffuseContribution;
    float3 sheen=saturate(layerSheen.rgb);float sr=max(.07,layerSheen.w);
    if(LayerTexture(3))sheen*=ShadingMaterialSample(sheenColorMap,uv,uvDx,uvDy).rgb;
    if(LayerTexture(4))sr=max(.07,sr*ShadingMaterialSample(sheenRoughnessMap,uv,uvDx,uvDy).a);
    if(max(sheen.r,max(sheen.g,sheen.b))>0&&probeOrigin.w>.5)
    {
        float4 eo=ShadingLinear(ltcAmplitudeMap,LtcUv(nv,sr)),ei=ShadingLinear(ltcAmplitudeMap,LtcUv(max(nl,.0001),sr));
        float inverseR=1/(sr*sr);
        float D=(2+inverseR)*pow(max(1-nh*nh,1e-8),.5*inverseR)/(2*pi);
        float V=1/max(4*(nl+nv-nl*nv),1e-5);
        float weight=max(sheen.r,max(sheen.g,sheen.b));
        result*=saturate(1-weight*max(eo.b,ei.b));
        result+=sheen*(D*V*nl/max(eo.a,1));
    }
    float cw=saturate(layerCoat.x),cr=max(.045,layerCoat.y);
    if(LayerTexture(0))cw*=ShadingMaterialSample(coatWeightMap,uv,uvDx,uvDy).r;
    if(LayerTexture(1))cr=max(.045,cr*ShadingMaterialSample(coatRoughnessMap,uv,uvDx,uvDy).g);
    if(cw>0)
    {
        float3 cn=geometricNormal*faceSign,ct=LayerTangent(cn,tangent),cb=cross(cn,ct)*bitangentSign;
        if(LayerTexture(2))
        {float3 mapped=ShadingMaterialSample(coatNormalMap,uv,uvDx,uvDy).xyz*2-1;mapped.xy*=layerCoat.z;cn=UnitVector(ct*mapped.x+cb*mapped.y+cn*mapped.z,cn);}
        cn*=faceSign;
        float cv=saturate(dot(cn,v)),cl=saturate(dot(cn,l)),ch=saturate(dot(cn,h));
        float a2=pow(cr,4),d=ch*ch*(a2-1)+1;
        float D=a2/max(pi*d*d,1e-12);
        float V=.5/max(cl*sqrt(cv*cv*(1-a2)+a2)+cv*sqrt(cl*cl*(1-a2)+a2),1e-8);
        float fv=.04+.96*pow(1-cv,5),fl=.04+.96*pow(1-cl,5),fh=.04+.96*pow(1-saturate(dot(v,h)),5);
        result*= (1-cw*fv)*(1-cw*fl);result+=cw*D*V*fh*cl;
    }
    // 各层权重为零、无直接补偿时恢复原算式，避免改变默认材质图。
    if(strength==0&&layerSubsurface.x==0&&cw==0&&all(sheen==0)&&shadingModes.x<.5)
        result=DirectBrdf(color,metal,rough,n,v,l);
    return max(result,0);
}
// 固定UV参考调用；生产PS显式传入其quad导数，不在变长灯循环内重新求导。
float3 SurfaceDirectBrdf(float3 color,float metal,float rough,float3 n,float3 geometricNormal,float3 v,float3 l,
    float3 tangent,float bitangentSign,float faceSign,float2 uv,float curvature)
{return SurfaceDirectBrdf(color,metal,rough,n,geometricNormal,v,l,tangent,bitangentSign,faceSign,uv,float2(0,0),float2(0,0),curvature);}
uint ClipLightHorizon(float3 source[8],uint count,out float3 target[8])
{
    [unroll]for(uint clearIndex=0;clearIndex<8;++clearIndex)target[clearIndex]=0;
    uint written=0;[loop]for(uint i=0;i<count;++i)
    {
        float3 a=source[i],b=source[(i+1)%count];
        if(a.z>=0&&written<8)target[written++]=a;
        if((a.z>=0)!=(b.z>=0)&&written<8)target[written++]=lerp(a,b,a.z/(a.z-b.z));
    }
    return written;
}
float RectangleCosineIntegral(uint light,float3 p,float3 n,float3 t,float3 b,float4 inverseLtc)
{
    float3 center=rectCenter[light].xyz,hu=rectHalfU[light].xyz,hv=rectHalfV[light].xyz;
    bool validGeometry=dot(cross(hu,hv),p-center)>0;
    float3 corners[4]={center-hu-hv-p,center-hu+hv-p,center+hu+hv-p,center+hu-hv-p};
    float3 polygon[8],clipped[8];[unroll]for(uint initIndex=0;initIndex<8;++initIndex){polygon[initIndex]=0;clipped[initIndex]=0;}
    [unroll]for(uint cornerIndex=0;cornerIndex<4;++cornerIndex)polygon[cornerIndex]=float3(dot(corners[cornerIndex],t),dot(corners[cornerIndex],b),dot(corners[cornerIndex],n));
    uint count=ClipLightHorizon(polygon,4,clipped);
    [loop]for(uint transformIndex=0;transformIndex<count;++transformIndex)
    {float3 q=clipped[transformIndex];polygon[transformIndex]=float3(inverseLtc.x*q.x+inverseLtc.z*q.z,q.y,inverseLtc.y*q.x+inverseLtc.w*q.z);}
    count=ClipLightHorizon(polygon,count,clipped);validGeometry=validGeometry&&count>=3;
    [loop]for(uint normalizeIndex=0;normalizeIndex<count;++normalizeIndex)
    {validGeometry=validGeometry&&dot(clipped[normalizeIndex],clipped[normalizeIndex])>=1e-20;clipped[normalizeIndex]=UnitVector(clipped[normalizeIndex],float3(0,0,1));}
    float sum=0;[loop]for(uint edgeIndex=0;edgeIndex<count;++edgeIndex)
    {float3 edgeA=clipped[edgeIndex],edgeB=clipped[(edgeIndex+1)%count],edgeCross=cross(edgeA,edgeB);float len=length(edgeCross);if(len>1e-8)sum+=edgeCross.z*atan2(len,clamp(dot(edgeA,edgeB),-1,1))/len;}
    return validGeometry?saturate(sum/(2*3.14159265359)):0;
}
float3 RectangleLighting(float3 color,float metal,float rough,float3 p,float3 n,float3 geometricNormal,float3 v,
    float3 tangent,float bitangentSign,float faceSign,float2 materialUv,float2 uvDx,float2 uvDy,float curvature)
{
    float3 t=LayerTangent(n,v),b=cross(n,t),f0=lerp(.04.xxx,color,metal);
    float2 uv=LtcUv(max(dot(n,v),.0001),rough);
    float4 ltcInverse=ShadingLinear(ltcMatrixMap,uv);float2 amplitude=ShadingLinear(ltcAmplitudeMap,uv).rg;
    float3 result=0;[loop]for(uint i=0;i<min((uint)shadingModes.w,4u);++i)
    {
        // 基础MR采用LTC；LTC的GGX单散射表不能冒充其它层/补偿的积分。
        // 这些显式组合采用固定8×8面积求积，复用同一表面BRDF，保留成本/精度边界供验收。
        if(shadingModes.x>.5||layerCoat.x>0||any(layerSheen.rgb>0)||layerAnisotropy.x>0||layerSubsurface.x>0)
        {
            float3 emitterCross=cross(rectHalfU[i].xyz,rectHalfV[i].xyz);
            float area=4*length(emitterCross);float3 emitterNormal=UnitVector(emitterCross,float3(0,0,1));
            if(dot(emitterNormal,p-rectCenter[i].xyz)<=0)continue;
            [loop]for(uint y=0;y<8;++y)[loop]for(uint x=0;x<8;++x)
            {
                float2 q=(float2(x,y)+.5)/8*2-1;
                float3 samplePosition=rectCenter[i].xyz+rectHalfU[i].xyz*q.x+rectHalfV[i].xyz*q.y;
                float3 delta=samplePosition-p;float squared=max(dot(delta,delta),1e-8),lscale=rsqrt(squared);float3 l=delta*lscale;
                float emitterCos=saturate(dot(emitterNormal,-l));
                result+=SurfaceDirectBrdf(color,metal,rough,n,geometricNormal,v,l,tangent,bitangentSign,faceSign,materialUv,uvDx,uvDy,curvature)
                    *rectRadiance[i].rgb*(area*emitterCos/(64*squared));
            }
            continue;
        }
        float diffuse=RectangleCosineIntegral(i,p,n,t,b,float4(1,0,0,1));
        float specular=RectangleCosineIntegral(i,p,n,t,b,ltcInverse);
        result+=rectRadiance[i].rgb*(color*(1-metal)*(1-f0)*diffuse+(f0*amplitude.x+(1-f0)*amplitude.y)*specular);
    }
    return max(result,0);
}
#endif
