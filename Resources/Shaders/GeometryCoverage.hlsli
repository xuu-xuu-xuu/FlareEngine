#ifndef SELF_ENGINE_GEOMETRY_COVERAGE
#define SELF_ENGINE_GEOMETRY_COVERAGE
// 宏展开保留原主 PS 的语法树，避免大 PCSS 程序因增加函数改变 FXC 优化选择。
// 两个 Pass 共用像素位置、Bayer 阈值和旧/新 LOD 的同一份互补覆盖定义。
#define ApplyLodCoverage(lodValue,pixelPosition) \
    const float lodDither=lodValue; \
    if(lodDither!=0.0&&abs(lodDither)<1.0) \
    { \
        const bool usesPrevious=abs(lodDither)<=LodBayerThreshold(uint2(pixelPosition)); \
        if((lodDither<0.0)!=usesPrevious)discard; \
    }
#endif
