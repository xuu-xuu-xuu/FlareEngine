// 光栅覆盖匹配当前深度；真实运动来自不带采样抖动的两帧投影。
cbuffer VelocityConstants : register(b0)
{
    float4x4 rasterClipFromLocal;
    float4x4 currentClipFromLocal;
    float4x4 previousClipFromLocal;
};
struct VelocityPixel
{
    float4 position : SV_POSITION;
    float4 currentClip : TEXCOORD0;
    float4 previousClip : TEXCOORD1;
};
float2 ProjectUv(float4 clip)
{
    // 不使用提前 return：D3DCompiler 在 /Od 流分析下会把提前返回误报为未初始化。
    const float w = clip.w > 1e-6f ? clip.w : 1e-6f;
    const float2 ndc = clip.xy / w;
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}
VelocityPixel VSMain(float4 position : POSITION)
{
    const float4 local = float4(position.xyz, 1.0f);
    VelocityPixel output;
    output.position = mul(rasterClipFromLocal, local);
    output.currentClip = mul(currentClipFromLocal, local);
    output.previousClip = mul(previousClipFromLocal, local);
    return output;
}
float2 PSMain(VelocityPixel input) : SV_TARGET
{
    // 在像素处做透视除法；先除再用透视插值会给倾斜的地面和细长三角形错误的速度。
    if (input.previousClip.w <= 1e-6f || input.currentClip.w <= 1e-6f) return float2(2, 2);
    return ProjectUv(input.previousClip) - ProjectUv(input.currentClip);
}
