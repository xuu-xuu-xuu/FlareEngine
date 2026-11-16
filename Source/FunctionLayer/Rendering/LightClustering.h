#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace se
{
// G09 原型：Forward+ 固定 16×9×24 集群。指数 Z 切分与 XY 视口划分在 CPU/GPU 共用同一公式；
// 先计数、再前缀和、最后填索引，容量不足时整体回退到全局灯列表（overflow）。
inline constexpr std::uint32_t LightClusterX=16,LightClusterY=9,LightClusterZ=24;
inline constexpr std::uint32_t LightClusterCount=LightClusterX*LightClusterY*LightClusterZ;
inline constexpr std::uint32_t LightClusterCapacity=65536;
struct LightClusterRange
{
    std::uint32_t offset=0,count=0;
};
struct LightClusterData
{
    std::vector<LightClusterRange> clusters; // 16*9*24，行优先 x + y*16 + z*16*9。
    std::vector<std::uint32_t> indices;
    bool overflow=false;
    std::uint32_t maxPerCluster=0;
};
struct LightClusterProjection
{
    Mat4 view,clipFromView; // world→view 与 view→clip。
};
// 失败不替换 output；capacity==0 使用默认 65536。
bool BuildLightClusters(std::span<const rhi::PointLight> lights,const LightClusterProjection& matrices,
    float nearPlane,float farPlane,LightClusterData& output,std::uint32_t capacity=LightClusterCapacity);
// 视空间深度 → Z 切片；非法输入返回 false。
bool LightClusterZSlice(float viewDepth,float nearPlane,float farPlane,std::uint32_t& slice);
// NDC → 集群索引；越界返回 false。
bool LightClusterIndex(float ndcX,float ndcY,float viewDepth,float nearPlane,float farPlane,std::uint32_t& index);
// 容量溢出的回退：把所有灯放入每个集群（全局灯列表）。
void FillLightClustersGlobal(std::uint32_t lightCount,LightClusterData& output);

// G09 GPU 路径的独立有界 CPU 参考。indices 始终指向调用者的灯数组，
// rawCounts 保留相交原数；局部截断和整表容量溢出分别留证，不伪装成无损。
struct BoundedLightClusterData
{
    LightClusterData table;
    std::vector<std::uint32_t> rawCounts,fallbackIndices;
    std::uint32_t truncatedClusters=0;
    std::uint64_t droppedReferences=0,requestedIndices=0;
};
// 最多256灯；稳定ID必须非零且唯一。每簇按到视空间 AABB 中心的平方距离排序，
// 等距按稳定ID。超全局容量时表为空、overflow=true，fallbackIndices为全局稳定前8。
// 失败保留 output；capacity=0 使用65536；maximumPerCluster只接受1..32。
bool BuildBoundedLightClusters(std::span<const rhi::PointLight> lights,std::span<const std::uint64_t> stableIds,
    const LightClusterProjection& matrices,float nearPlane,float farPlane,BoundedLightClusterData& output,
    std::uint32_t capacity=LightClusterCapacity,std::uint32_t maximumPerCluster=32);
}
