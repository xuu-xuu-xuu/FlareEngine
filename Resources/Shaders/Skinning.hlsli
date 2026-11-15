#ifndef SELF_ENGINE_SKINNING
#define SELF_ENGINE_SKINNING
// W03：蒙皮调色板。约定与 MaterialLayout.h / rhi::SkinnedVertex 一致：
// 每关节 3 行 float4（行主序 3×4 的实体局部蒙皮矩阵），共 48 字节；
// 关节序号来自 JOINTS0，权重来自 WEIGHTS0，都是逐顶点属性。
// 调色板由 ByteAddressBuffer 提供，寄存器固定为 t8：
//   受光家族（31 号绑定布局）在根参数 4，字节偏移由 PbrConstants.skin.x 给出；
//   阴影深度在根参数 1，绑定范围就是单个调色板，偏移固定为 0。
ByteAddressBuffer bones : register(t8);
// 目标空间 From 蒙皮前空间的局部矩阵：第 4 行补 (0,0,0,1)，供 float4 点乘使用。
float4x4 LoadSkinMatrix(uint paletteByteOffset,uint joint)
{
    const uint address=paletteByteOffset+joint*48;
    return float4x4(asfloat(bones.Load4(address)),asfloat(bones.Load4(address+16)),
        asfloat(bones.Load4(address+32)),float4(0,0,0,1));
}
// 四路影响按权重线性混合；无影响的槽位权重为 0，直接跳过读取。
float4x4 WeightedSkinMatrix(uint paletteByteOffset,uint4 joints,float4 weights)
{
    float4x4 result=(float4x4)0;
    [unroll] for(uint influence=0;influence<4;++influence)
    {
        const float weight=weights[influence];
        if(abs(weight)<1e-8)continue;
        result+=LoadSkinMatrix(paletteByteOffset,joints[influence])*weight;
    }
    return result;
}
// 蒙皮结果仍在实体局部空间，随后由 world/clip 矩阵变换；法线与切线用 3×3 线性部分。
float3 SkinPosition(float4x4 skinFromSkinned,float3 position)
{
    return mul(skinFromSkinned,float4(position,1)).xyz;
}
float3 SkinDirection(float4x4 skinFromSkinned,float3 direction)
{
    return mul((float3x3)skinFromSkinned,direction);
}
#endif
