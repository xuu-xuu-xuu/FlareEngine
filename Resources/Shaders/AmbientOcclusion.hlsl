// GTAO horizon 积分参考 Intel XeGTAO；独立的 SM5.1/逆投影/采样与资源实现。
// 第三方数学实现参考的许可保留于 AmbientOcclusion.LICENSE.txt。
#include "AmbientOcclusionBindings.hlsli"

float AoArc(float angle,float normalAngle,float cosineNormal)
{return .25*(cosineNormal+2*angle*sin(normalAngle)-cos(2*angle-normalAngle));}
float AoHorizon(int2 pixel,float3 center,float3 normal,float3 viewDirection,float radius,float baseline)
{
    float result=baseline;float3 samplePosition=0;
    if(AoReadPosition(pixel,samplePosition))
    {
        float3 delta=samplePosition-center;float squared=dot(delta,delta);
        if(squared>1e-12&&squared<radius*radius&&dot(delta,normal)>aoParameters.z)
        {
            float distance=sqrt(squared);float weight=saturate(1-distance/radius);
            result=lerp(baseline,dot(delta,viewDirection)/distance,weight);
        }
    }
    return result;
}
[numthreads(8,8,1)]
void GtaoHalf(uint3 threadId:SV_DispatchThreadID)
{
    if(any(threadId.xy>=(uint2)aoViewport.zw))return;
    int2 pixel=min(int2(threadId.xy*2),int2(aoViewport.xy)-1);float3 center=0,normal=0;
    bool valid=AoReadSurface(pixel,center,normal);float result=1;
    if(valid&&aoParameters.y>0)
    {
        float3 nearPosition=0,midPosition=0,neighborX=0,neighborY=0;
        float depth=prepassDepth.Load(int3(pixel,0));
        bool basis=AoUnproject(float2(pixel),0,nearPosition)&&AoUnproject(float2(pixel),.5,midPosition)
            &&AoUnproject(float2(pixel)+float2(1,0),depth,neighborX)&&AoUnproject(float2(pixel)+float2(0,1),depth,neighborY);
        if(basis)
        {
            float3 viewDirection=AoUnit(nearPosition-midPosition,AoUnit(-center,float3(0,0,1)));
            float3 pixelX=neighborX-center,pixelY=neighborY-center;
            float2 projectedRadius=min(aoParameters.x/max(float2(length(pixelX),length(pixelY)),1e-6),aoViewport.xy*2);
            float rotation=frac(52.9829189*frac(.06711056*pixel.x+.00583715*pixel.y))*AoPi;
            float visibility=0,unoccluded=0;
            // 四个 slice；每侧八步，总计 64 个深度 tap，不隐藏双侧成本。
            [unroll]for(uint sliceIndex=0;sliceIndex<4;++sliceIndex)
            {
                float angle=rotation+AoPi*sliceIndex/4;float2 direction=float2(cos(angle),sin(angle));
                float3 sliceDirection=pixelX*direction.x+pixelY*direction.y;
                sliceDirection=AoUnit(sliceDirection-viewDirection*dot(sliceDirection,viewDirection),float3(1,0,0));
                float3 axis=AoUnit(cross(sliceDirection,viewDirection),float3(0,1,0));
                float3 projectedNormal=normal-axis*dot(normal,axis);float normalLength=length(projectedNormal);
                if(normalLength>1e-5&&dot(normal,viewDirection)>0)
                {
                    float normalAngle=atan2(dot(projectedNormal,sliceDirection),dot(projectedNormal,viewDirection));
                    float cosineNormal=dot(projectedNormal,viewDirection)/normalLength;
                    float basePlus=cos(clamp(normalAngle+AoPi*.5,0,AoPi*.5));
                    float baseMinus=cos(clamp(AoPi*.5-normalAngle,0,AoPi*.5));
                    float plus=basePlus,minus=baseMinus;
                    [unroll]for(uint radiusStep=0;radiusStep<8;++radiusStep)
                    {
                        float fraction=(radiusStep+.5)/8;fraction*=fraction;
                        int2 offset=(int2)round(direction*max(projectedRadius*fraction,float2(1,1)));
                        if(any(offset!=0))
                        {
                            plus=max(plus,AoHorizon(pixel+offset,center,normal,viewDirection,aoParameters.x,basePlus));
                            minus=max(minus,AoHorizon(pixel-offset,center,normal,viewDirection,aoParameters.x,baseMinus));
                        }
                    }
                    float h0=-acos(clamp(minus,-1,1)),h1=acos(clamp(plus,-1,1));
                    float b0=-acos(clamp(baseMinus,-1,1)),b1=acos(clamp(basePlus,-1,1));
                    visibility+=normalLength*(AoArc(h0,normalAngle,cosineNormal)+AoArc(h1,normalAngle,cosineNormal));
                    unoccluded+=normalLength*(AoArc(b0,normalAngle,cosineNormal)+AoArc(b1,normalAngle,cosineNormal));
                }
            }
            // 无遮挡的倾斜平面也归一到1；不靠人为抬黑位掩盖法线/投影错误。
            float ratio=unoccluded>1e-5?saturate(visibility/unoccluded):1;
            result=saturate(1-(1-ratio)*aoParameters.y);
        }
    }
    halfOutput[threadId.xy]=float2(result,valid?1:0);
}
float AoUpsample(int2 pixel,float3 center,float3 normal)
{
    float result=1,weighted=0,total=0;int2 halfCenter=pixel>>1;
    float depthLimit=max(aoParameters.z*2+.001,min(aoParameters.x*.25,abs(center.z)*.02));
    [unroll]for(int tapY=-2;tapY<=2;++tapY)
    {
        [unroll]for(int tapX=-2;tapX<=2;++tapX)
        {
            int2 halfPixel=halfCenter+int2(tapX,tapY);
            if(all(halfPixel>=0)&&all(halfPixel<int2(aoViewport.zw)))
            {
                float2 value=halfOcclusion.Load(int3(halfPixel,0));float3 samplePosition=0,sampleNormal=0;
                if(value.y>.5&&AoReadSurface(min(halfPixel*2,int2(aoViewport.xy)-1),samplePosition,sampleNormal))
                {
                    float difference=abs(samplePosition.z-center.z),agreement=dot(normal,sampleNormal);
                    // 任一边界失配就拒绝，不能用AND把前景/背景或薄片边缘混在一起。
                    if(difference<=depthLimit&&agreement>=.8)
                    {
                        float weight=exp2(-4*difference/max(depthLimit,1e-6))*pow(saturate(agreement),8)/(1+tapX*tapX+tapY*tapY);
                        // 累加遮挡量而非可见度：常量白输入恒为0，避免a/a倒数优化后半精度变暗。
                        weighted+=(1-saturate(value.x))*weight;total+=weight;
                    }
                }
            }
        }
    }
    if(total>1e-6)result=saturate(1-weighted/total);return result;
}
float AoContact(int2 screenPixel,float3 center,float3 normal)
{
    float ign=frac(52.9829189*frac(dot(float2(screenPixel),float2(0.06711056,0.00583715))));
    float result=1;float3 origin=center+normal*contactParameters.y;
    [loop]for(uint contactStep=1;contactStep<=12;++contactStep)
    {
        float fraction=max(contactStep-1.0+ign,0.05)/12.0;float3 ray=origin+aoLightDirection.xyz*(contactParameters.x*fraction);int2 pixel=0;
        if(!AoProject(ray,pixel))break;
        float3 surface=0;
        if(AoReadPosition(pixel,surface))
        {
            float difference=surface.z-ray.z;
            if(difference>contactParameters.y&&difference<contactParameters.z&&dot(surface-center,normal)>contactParameters.y)
            {
                // fade是射线末端淡出比例；不是把整个射线前90%都误判为无阴影。
                float shadow=contactParameters.w>0?saturate((1-fraction)/contactParameters.w):1;
                result=1-shadow;break;
            }
        }
    }
    return result;
}
[numthreads(8,8,1)]
void ResolveOcclusion(uint3 threadId:SV_DispatchThreadID)
{
    if(any(threadId.xy>=(uint2)aoViewport.xy))return;
    float3 center=0,normal=0;float2 visibility=1;
    if(AoReadSurface(int2(threadId.xy),center,normal))
    {
        if(aoModes.x>.5&&aoParameters.y>0)visibility.x=AoUpsample(int2(threadId.xy),center,normal);
        if(aoModes.y>.5&&aoLightDirection.w>.5)visibility.y=AoContact(int2(threadId.xy),center,normal);
    }
    visibilityOutput[threadId.xy]=saturate(visibility);
}
