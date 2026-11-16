cbuffer HeterogeneousParameters : register(b0)
{
    float4 planes[6];
    uint commandCount;
    uint3 reserved;
    column_major float4x4 clipFromWorld;
    float4 screen; // 当前帧HZB width/height/depthBias/mipCount。
    uint occlusionEnabled;
    uint3 occlusionReserved;
};
#include "VisibilityPredicate.hlsli"
StructuredBuffer<VisibilityItem> inputBounds : register(t0);
RWByteAddressBuffer outputCommands : register(u0);
Texture2D<float> currentFrameHiZ : register(t1);

// 与既有 Indirect 路径相同的 max-HZB 保守规则；只使用已构建的当前帧深度。
// 任何角跨近平面、屏幕外、深度越界、非有限或 floor mip 未覆盖边缘均保留可见。
bool HiddenByCurrentFrame(VisibilityItem item)
{
    bool hidden=false;
    if(occlusionEnabled!=0)
    {
        float2 minimum=1e30,maximum=-1e30;float nearest=1;bool valid=true;
        [unroll]for(uint cornerIndex=0;cornerIndex<8;++cornerIndex)
        {
            float3 position=float3((cornerIndex&1)?item.maximum.x:item.minimum.x,
                (cornerIndex&2)?item.maximum.y:item.minimum.y,(cornerIndex&4)?item.maximum.z:item.minimum.z);
            float4 clip=mul(clipFromWorld,float4(position,1));float3 ndc=0;
            bool cornerValid=all(isfinite(clip))&&clip.w>1e-6;
            if(cornerValid){ndc=clip.xyz/clip.w;cornerValid=all(isfinite(ndc))&&ndc.z>=0&&ndc.z<=1;}
            valid=valid&&cornerValid;
            if(cornerValid){minimum=min(minimum,ndc.xy);maximum=max(maximum,ndc.xy);nearest=min(nearest,ndc.z);}
        }
        valid=valid&&all(minimum>=-1)&&all(maximum<=1);
        if(valid)
        {
            float2 lo=float2(minimum.x*.5+.5,.5-maximum.y*.5)*screen.xy;
            float2 hi=float2(maximum.x*.5+.5,.5-minimum.y*.5)*screen.xy;
            lo=max(0,lo-.01);hi=min(screen.xy,hi+.01);
            if(all(hi>lo))
            {
                uint level=0;float scale=1;uint levels=(uint)screen.w;
                [loop]while(level+1<levels&&any((hi-lo)/scale>1.99)){++level;scale*=2;}
                uint2 dimensions=max(uint2(1,1),(uint2)screen.xy>>level);
                if(all(hi<=float2(dimensions)*scale+.0001))
                {
                    uint2 first=min((uint2)floor(lo/scale),dimensions-1);
                    uint2 last=min((uint2)max(0,ceil(hi/scale)-1),dimensions-1);float farthest=0;
                    [unroll]for(uint sampleY=0;sampleY<4;++sampleY)
                    {
                        [unroll]for(uint sampleX=0;sampleX<4;++sampleX)
                        {
                            uint2 texel=first+uint2(sampleX,sampleY);
                            if(all(texel<=last))farthest=max(farthest,currentFrameHiZ.Load(int3(texel,level)));
                        }
                    }
                    hidden=isfinite(farthest)&&nearest>farthest+screen.z;
                }
            }
        }
    }
    return hidden;
}

// 命令模板已由 COPY 写入：VBV16 + IBV16 + CBV8 + DRAW_INDEXED20 + pad4。
// 固定输入次序；不 append、不压紧，也不修改地址、索引范围或实例起点。
[numthreads(64,1,1)]
void Cull(uint3 thread : SV_DispatchThreadID)
{
    if(thread.x<commandCount)
        outputCommands.Store(thread.x*64+44,Visible(inputBounds[thread.x])&&!HiddenByCurrentFrame(inputBounds[thread.x])?1u:0u);
}
