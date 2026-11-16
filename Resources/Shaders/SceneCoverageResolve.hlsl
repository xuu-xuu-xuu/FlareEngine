#include "Fullscreen.hlsli"
Texture2D<float4> sceneRadiance : register(t0);
Texture2D<float> sceneDepth : register(t1);

struct ResolvedPixel { float4 color : SV_Target; float depth : SV_Depth; };
ResolvedPixel PSMain(float4 position : SV_Position)
{
    const int2 base = int2(position.xy) * 2;
    ResolvedPixel pixel;
    pixel.color = 0;
    pixel.depth = 1;
    [unroll] for (int y=0; y<2; ++y)
        [unroll] for (int x=0; x<2; ++x)
        {
            int3 sampleAt = int3(base + int2(x,y), 0);
            pixel.color += sceneRadiance.Load(sampleAt) * 0.25;
            // 常规 Z：保留最靠近相机的表面供后处理与编辑器遮挡测试。
            pixel.depth = min(pixel.depth, sceneDepth.Load(sampleAt));
        }
    return pixel;
}
