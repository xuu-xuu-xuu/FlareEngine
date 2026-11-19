#pragma once
#include "CoreLayer/Math/Bounds.h"
#include "CoreLayer/Math/Projection.h"
#include "FunctionLayer/Runtime/RuntimeHandles.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace se
{
struct StaticBatchCellCoord
{
    std::int32_t x=0,y=0,z=0;
    auto operator<=>(const StaticBatchCellCoord&) const = default;
};

StaticBatchCellCoord StaticBatchCellFor(const Vec3& worldPosition,float cellSizeMeters) noexcept;

struct StaticBatchKey
{
    std::uint32_t materialLayoutVersion=0;
    std::uint64_t pipelineKey=0;
    std::uint32_t vertexFormatVersion=rhi::VertexLayoutVersionLegacy;
    std::uint64_t materialBindingKey=0;
    auto operator<=>(const StaticBatchKey&) const = default;
};

StaticBatchKey MakeStaticBatchKey(const rhi::DrawIndexed& prototype,
    std::uint32_t materialLayoutVersion=0,
    std::uint32_t vertexFormatVersion=rhi::VertexLayoutVersionLegacy) noexcept;

struct StaticBatchSubmeshInput
{
    std::uint32_t submeshSlot=0;
    StaticBatchKey key{};
    rhi::DrawIndexed prototype{};
    std::span<const rhi::Vertex> localVertices{};
    std::span<const std::uint32_t> localIndices{};
    Mat4 world=Mat4::Identity();
};

struct StaticBatchSubmeshRange
{
    std::uint32_t firstIndex=0,indexCount=0;
    std::uint32_t firstVertex=0,vertexCount=0;
    StableEntityId entityId=0;
    std::uint32_t submeshSlot=0;
    Aabb worldBounds{};
};

struct StaticBatchEntry
{
    StaticBatchCellCoord cell{};
    StaticBatchKey key{};
    rhi::Buffer vertices;
    rhi::Buffer indices;
    rhi::DrawIndexed draw{};
    Aabb worldBounds{};
    std::uint32_t vertexCount=0,indexCount=0;
    std::vector<StaticBatchSubmeshRange> ranges;
};

struct StaticBatchPickHit
{
    StableEntityId entityId=0;
    std::uint32_t submeshSlot=0;
    std::size_t batchIndex=0;
    std::uint32_t triangleIndex=0;
    float distance=0.f;
    Vec3 worldPoint{};
};

struct StaticBatchStatistics
{
    std::size_t entityCount=0;
    std::size_t activeCells=0;
    std::size_t batchCount=0;
    std::size_t sourceSubmeshes=0;
    std::size_t lastRebuiltCells=0;
    std::size_t lastCachedCells=0;
    std::uint64_t totalCellRebuilds=0;
    std::uint64_t mergedVertexBytes=0;
    std::uint64_t mergedIndexBytes=0;
};

struct StaticBatchWorkloadProfile
{
    std::uint32_t staticDrawCount=0;
    std::uint32_t instancedBatchCount=0;
    std::uint32_t staticBatchCount=0;
    std::uint64_t mergedBufferBytes=0;
    std::uint64_t vramBudgetBytes=64ull*1024ull*1024ull;
    double perObjectSubmitMs=0.0;
    double instancedSubmitMs=0.0;
    double staticBatchSubmitMs=0.0;
};

// RG104-14：仅在合并批次显著少于 GPU 实例化批次、显存预算允许且实测提交耗时取得正收益时启用。
bool ShouldEnableStaticBatching(const StaticBatchWorkloadProfile& profile) noexcept;

class StaticBatchCache
{
public:
    explicit StaticBatchCache(float cellSizeMeters=32.f);

    bool ConfigureCellSize(float cellSizeMeters);
    float CellSizeMeters() const noexcept { return cellSizeMeters_; }

    bool UpsertEntity(StableEntityId entityId,std::span<const StaticBatchSubmeshInput> submeshes,std::uint64_t attributeRevision=0);
    bool RemoveEntity(StableEntityId entityId);
    void InvalidateCell(StaticBatchCellCoord cell);
    void Clear();

    rhi::Result RebuildDirty(rhi::Device& device);

    std::span<const StaticBatchEntry> Batches() const noexcept { return flatBatches_; }
    const StaticBatchStatistics& Stats() const noexcept { return stats_; }

    StableEntityId ResolveEntityByTriangle(std::size_t batchIndex,std::uint32_t triangleIndex,std::uint32_t* outSubmeshSlot=nullptr) const noexcept;
    std::optional<StaticBatchPickHit> RaycastPick(const Vec3& rayOrigin,const Vec3& rayDirection,float maxDistance=1e6f) const;

private:
    struct StoredSubmesh
    {
        StableEntityId entityId=0;
        std::uint32_t submeshSlot=0;
        StaticBatchCellCoord cell{};
        StaticBatchKey key{};
        rhi::DrawIndexed prototype{};
        std::vector<rhi::Vertex> worldVertices;
        std::vector<std::uint32_t> localIndices;
        Aabb worldBounds{};
        std::uint64_t contentHash=0;
    };
    struct EntityRecord
    {
        std::uint64_t attributeRevision=0;
        std::uint64_t aggregateHash=0;
        std::vector<StoredSubmesh> submeshes;
    };
    struct CellBatchCpuGeometry
    {
        std::vector<rhi::Vertex> vertices;
        std::vector<std::uint32_t> indices;
    };
    struct CellRecord
    {
        bool dirty=true;
        std::vector<StaticBatchEntry> batches;
        std::vector<CellBatchCpuGeometry> cpuGeometry;
    };

    void MarkDirty(StaticBatchCellCoord cell);
    void RefreshFlatView();

    float cellSizeMeters_=32.f;
    std::unordered_map<StableEntityId,EntityRecord> entities_;
    std::map<StaticBatchCellCoord,CellRecord> cells_;
    std::vector<StaticBatchEntry> flatBatches_;
    std::vector<std::pair<StaticBatchCellCoord,std::size_t>> flatCpuRefs_;
    StaticBatchStatistics stats_{};
};
}
