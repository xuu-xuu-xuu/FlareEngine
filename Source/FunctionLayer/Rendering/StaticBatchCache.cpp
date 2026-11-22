#include "StaticBatchCache.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace flare
{
namespace
{
std::uint32_t CanonicalBits(float v) noexcept
{
    if(v==0.f)return 0u;
    std::uint32_t bits=0;
    std::memcpy(&bits,&v,sizeof(bits));
    return bits;
}
std::uint64_t Mix64(std::uint64_t seed,std::uint64_t value) noexcept
{
    value+=0x9E3779B97F4A7C15ull;
    value=(value^(value>>30))*0xBF58476D1CE4E5B9ull;
    value=(value^(value>>27))*0x94D049BB133111EBull;
    value^=(value>>31);
    return seed^(value+0x9E3779B97F4A7C15ull+(seed<<6)+(seed>>2));
}
template<class Tag>
std::uint64_t PackHandle(rhi::Handle<Tag> h) noexcept
{
    return (static_cast<std::uint64_t>(h.generation)<<32)|static_cast<std::uint64_t>(h.index);
}
bool RayIntersectsAabb(const Vec3& origin,const Vec3& invDir,const Aabb& box,float maxDist) noexcept
{
    float tmin=0.f,tmax=maxDist;
    const float o[3]{origin.x,origin.y,origin.z};
    const float id[3]{invDir.x,invDir.y,invDir.z};
    const float bmin[3]{box.minimum.x,box.minimum.y,box.minimum.z};
    const float bmax[3]{box.maximum.x,box.maximum.y,box.maximum.z};
    for(int axis=0;axis<3;++axis)
    {
        if(std::abs(id[axis])>1e20f)
        {
            if(o[axis]<bmin[axis]||o[axis]>bmax[axis])return false;
            continue;
        }
        float t1=(bmin[axis]-o[axis])*id[axis];
        float t2=(bmax[axis]-o[axis])*id[axis];
        if(t1>t2)std::swap(t1,t2);
        tmin=(std::max)(tmin,t1);
        tmax=(std::min)(tmax,t2);
        if(tmin>tmax)return false;
    }
    return true;
}
bool RayIntersectsTriangle(const Vec3& origin,const Vec3& dir,const Vec3& v0,const Vec3& v1,const Vec3& v2,
    float maxDist,float& outT) noexcept
{
    const Vec3 e1=v1-v0;
    const Vec3 e2=v2-v0;
    const Vec3 pvec=Cross(dir,e2);
    const float det=Dot(e1,pvec);
    if(std::abs(det)<1e-8f)return false;
    const float invDet=1.f/det;
    const Vec3 tvec=origin-v0;
    const float u=Dot(tvec,pvec)*invDet;
    if(u<0.f||u>1.f)return false;
    const Vec3 qvec=Cross(tvec,e1);
    const float v=Dot(dir,qvec)*invDet;
    if(v<0.f||u+v>1.f)return false;
    const float t=Dot(e2,qvec)*invDet;
    if(t<0.f||t>maxDist)return false;
    outT=t;
    return true;
}
}

StaticBatchCellCoord StaticBatchCellFor(const Vec3& worldPosition,float cellSizeMeters) noexcept
{
    if(!(cellSizeMeters>0.f)||!std::isfinite(cellSizeMeters)||!IsFinite(worldPosition))return {};
    return {
        static_cast<std::int32_t>(std::floor(worldPosition.x/cellSizeMeters)),
        static_cast<std::int32_t>(std::floor(worldPosition.y/cellSizeMeters)),
        static_cast<std::int32_t>(std::floor(worldPosition.z/cellSizeMeters))
    };
}

StaticBatchKey MakeStaticBatchKey(const rhi::DrawIndexed& prototype,
    std::uint32_t materialLayoutVersion,
    std::uint32_t vertexFormatVersion) noexcept
{
    std::uint64_t mat=0x243F6A8885A308D3ull;
    mat=Mix64(mat,PackHandle(prototype.texture));
    mat=Mix64(mat,PackHandle(prototype.normalTexture));
    mat=Mix64(mat,PackHandle(prototype.metallicRoughnessTexture));
    mat=Mix64(mat,CanonicalBits(prototype.tint.x));
    mat=Mix64(mat,CanonicalBits(prototype.tint.y));
    mat=Mix64(mat,CanonicalBits(prototype.tint.z));
    mat=Mix64(mat,CanonicalBits(prototype.tint.w));
    mat=Mix64(mat,CanonicalBits(prototype.pbr.metallic));
    mat=Mix64(mat,CanonicalBits(prototype.pbr.roughness));
    mat=Mix64(mat,CanonicalBits(prototype.pbr.normalScale));
    mat=Mix64(mat,prototype.pbr.normalMapping?1ull:0ull);
    mat=Mix64(mat,prototype.pbr.doubleSided?1ull:0ull);
    mat=Mix64(mat,prototype.pbr.receivesShadow?1ull:0ull);
    mat=Mix64(mat,prototype.pbr.multiscatter?1ull:0ull);
    mat=Mix64(mat,prototype.pbr.specularAA?1ull:0ull);
    mat=Mix64(mat,prototype.unlit?1ull:0ull);
    mat=Mix64(mat,prototype.textureIsData?1ull:0ull);
    mat=Mix64(mat,static_cast<std::uint64_t>(prototype.alphaMode));
    mat=Mix64(mat,CanonicalBits(prototype.alphaCutoff));
    for(const Vec4& v:{prototype.layers.constants.coat,prototype.layers.constants.sheen,
        prototype.layers.constants.anisotropy,prototype.layers.constants.subsurface,
        prototype.layers.constants.subsurfaceColor,prototype.layers.constants.textureFlags})
    {
        mat=Mix64(mat,CanonicalBits(v.x));
        mat=Mix64(mat,CanonicalBits(v.y));
        mat=Mix64(mat,CanonicalBits(v.z));
        mat=Mix64(mat,CanonicalBits(v.w));
    }
    for(const auto& tex:prototype.layers.textures)mat=Mix64(mat,PackHandle(tex));
    return {materialLayoutVersion,PackHandle(prototype.pipeline),vertexFormatVersion,mat};
}

bool ShouldEnableStaticBatching(const StaticBatchWorkloadProfile& profile) noexcept
{
    if(profile.staticDrawCount<2||profile.staticBatchCount==0)return false;
    if(profile.mergedBufferBytes>profile.vramBudgetBytes)return false;
    // 若 GPU 实例化已经能把同网格重复物体压缩到相近或更少的批次数，优先保留零几何复制的实例化路径。
    if(profile.instancedBatchCount>0&&profile.staticBatchCount*4>=profile.instancedBatchCount*3)return false;
    if(profile.staticBatchCount>=profile.staticDrawCount)return false;
    if(profile.staticBatchSubmitMs>0.0&&(profile.instancedSubmitMs>0.0||profile.perObjectSubmitMs>0.0))
    {
        double bestAlternative=std::numeric_limits<double>::infinity();
        if(profile.perObjectSubmitMs>0.0)bestAlternative=(std::min)(bestAlternative,profile.perObjectSubmitMs);
        if(profile.instancedSubmitMs>0.0)bestAlternative=(std::min)(bestAlternative,profile.instancedSubmitMs);
        return profile.staticBatchSubmitMs<bestAlternative*0.95;
    }
    return true;
}

StaticBatchCache::StaticBatchCache(float cellSizeMeters)
{
    ConfigureCellSize(cellSizeMeters);
}

bool StaticBatchCache::ConfigureCellSize(float cellSizeMeters)
{
    if(!std::isfinite(cellSizeMeters)||cellSizeMeters<1.f||cellSizeMeters>4096.f)return false;
    if(std::abs(cellSizeMeters_-cellSizeMeters)<1e-6f)return true;
    cellSizeMeters_=cellSizeMeters;
    std::map<StaticBatchCellCoord,CellRecord> rebuilt;
    for(auto& [_,record]:entities_)
    {
        for(auto& sub:record.submeshes)
        {
            const Vec3 center=(sub.worldBounds.minimum+sub.worldBounds.maximum)*0.5f;
            sub.cell=StaticBatchCellFor(center,cellSizeMeters_);
            rebuilt[sub.cell].dirty=true;
        }
    }
    cells_=std::move(rebuilt);
    flatBatches_.clear();
    flatCpuRefs_.clear();
    return true;
}

void StaticBatchCache::MarkDirty(StaticBatchCellCoord cell)
{
    cells_[cell].dirty=true;
}

bool StaticBatchCache::UpsertEntity(StableEntityId entityId,std::span<const StaticBatchSubmeshInput> submeshes,std::uint64_t attributeRevision)
{
    if(!entityId||submeshes.empty())return false;

    EntityRecord next;
    next.attributeRevision=attributeRevision;
    std::uint64_t agg=Mix64(0x9E3779B97F4A7C15ull,attributeRevision);
    next.submeshes.reserve(submeshes.size());

    for(const auto& input:submeshes)
    {
        if(input.localVertices.empty()||input.localIndices.empty()||(input.localIndices.size()%3)!=0
            ||!IsFinite(input.world))return false;
        const auto inverse=Inverse(input.world);
        if(!inverse)return false;
        for(const auto idx:input.localIndices)
            if(idx>=input.localVertices.size())return false;

        const Mat4 normalMatrix=Transpose(*inverse);
        const Vec3 axisX{input.world(0,0),input.world(1,0),input.world(2,0)};
        const Vec3 axisY{input.world(0,1),input.world(1,1),input.world(2,1)};
        const Vec3 axisZ{input.world(0,2),input.world(1,2),input.world(2,2)};
        const float handednessSign=Dot(Cross(axisX,axisY),axisZ)<0.f?-1.f:1.f;

        StoredSubmesh stored;
        stored.entityId=entityId;
        stored.submeshSlot=input.submeshSlot;
        stored.key=input.key;
        stored.prototype=input.prototype;
        stored.worldVertices.resize(input.localVertices.size());
        stored.localIndices.assign(input.localIndices.begin(),input.localIndices.end());
        if(handednessSign<0.f)
        {
            for(std::size_t tri=0;tri+2<stored.localIndices.size();tri+=3)
                std::swap(stored.localIndices[tri+1],stored.localIndices[tri+2]);
        }

        Vec3 minP{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
        Vec3 maxP{-minP.x,-minP.y,-minP.z};
        for(std::size_t i=0;i<input.localVertices.size();++i)
        {
            const auto& src=input.localVertices[i];
            auto& dst=stored.worldVertices[i];
            const Vec3 wp=TransformPoint(input.world,{src.position[0],src.position[1],src.position[2]});
            if(!IsFinite(wp))return false;
            dst.position={wp.x,wp.y,wp.z};
            minP={(std::min)(minP.x,wp.x),(std::min)(minP.y,wp.y),(std::min)(minP.z,wp.z)};
            maxP={(std::max)(maxP.x,wp.x),(std::max)(maxP.y,wp.y),(std::max)(maxP.z,wp.z)};

            Vec3 wn=TransformVector(normalMatrix,{src.normal[0],src.normal[1],src.normal[2]});
            const float nLen=Length(wn);
            wn=nLen>1e-8f?(wn/nLen):Vec3{0.f,1.f,0.f};
            dst.normal={wn.x,wn.y,wn.z};

            dst.uv=src.uv;

            Vec3 wt=TransformVector(input.world,{src.tangent[0],src.tangent[1],src.tangent[2]});
            const float tLen=Length(wt);
            wt=tLen>1e-8f?(wt/tLen):Vec3{1.f,0.f,0.f};
            dst.tangent={wt.x,wt.y,wt.z,src.tangent[3]*handednessSign};
        }
        stored.worldBounds={minP,maxP};
        stored.cell=StaticBatchCellFor((minP+maxP)*0.5f,cellSizeMeters_);

        std::uint64_t h=Mix64(0x517CC1B727220A95ull,input.submeshSlot);
        h=Mix64(h,input.key.materialLayoutVersion);
        h=Mix64(h,input.key.pipelineKey);
        h=Mix64(h,input.key.vertexFormatVersion);
        h=Mix64(h,input.key.materialBindingKey);
        for(const float v:input.world.values)h=Mix64(h,CanonicalBits(v));
        h=Mix64(h,input.localVertices.size());
        h=Mix64(h,input.localIndices.size());
        stored.contentHash=h;
        agg=Mix64(agg,h);
        next.submeshes.push_back(std::move(stored));
    }
    next.aggregateHash=agg;

    auto found=entities_.find(entityId);
    if(found!=entities_.end()&&found->second.aggregateHash==next.aggregateHash
        &&found->second.submeshes.size()==next.submeshes.size())
    {
        bool same=true;
        for(std::size_t i=0;i<next.submeshes.size();++i)
        {
            if(found->second.submeshes[i].cell!=next.submeshes[i].cell
                ||found->second.submeshes[i].key!=next.submeshes[i].key
                ||found->second.submeshes[i].contentHash!=next.submeshes[i].contentHash)
            {
                same=false;
                break;
            }
        }
        if(same)return true;
    }

    if(found!=entities_.end())
    {
        for(const auto& oldSub:found->second.submeshes)MarkDirty(oldSub.cell);
    }
    for(const auto& newSub:next.submeshes)MarkDirty(newSub.cell);
    entities_.insert_or_assign(entityId,std::move(next));
    return true;
}

bool StaticBatchCache::RemoveEntity(StableEntityId entityId)
{
    const auto found=entities_.find(entityId);
    if(found==entities_.end())return false;
    for(const auto& sub:found->second.submeshes)MarkDirty(sub.cell);
    entities_.erase(found);
    return true;
}

void StaticBatchCache::InvalidateCell(StaticBatchCellCoord cell)
{
    MarkDirty(cell);
}

void StaticBatchCache::Clear()
{
    entities_.clear();
    cells_.clear();
    flatBatches_.clear();
    flatCpuRefs_.clear();
    stats_={};
}

rhi::Result StaticBatchCache::RebuildDirty(rhi::Device& device)
{
    std::map<StaticBatchCellCoord,std::vector<const StoredSubmesh*>> cellInputs;
    std::size_t totalSubmeshes=0;
    for(const auto& [_,record]:entities_)
    {
        for(const auto& sub:record.submeshes)
        {
            cellInputs[sub.cell].push_back(&sub);
            ++totalSubmeshes;
        }
    }

    std::size_t rebuiltCells=0,cachedCells=0;
    for(auto it=cells_.begin();it!=cells_.end();)
    {
        const auto cellCoord=it->first;
        auto& cell=it->second;
        const auto inputIt=cellInputs.find(cellCoord);
        const bool hasMembers=(inputIt!=cellInputs.end()&&!inputIt->second.empty());
        if(!cell.dirty)
        {
            if(hasMembers)
            {
                ++cachedCells;
                ++it;
            }
            else
            {
                it=cells_.erase(it);
            }
            continue;
        }

        ++rebuiltCells;
        ++stats_.totalCellRebuilds;
        if(!hasMembers)
        {
            it=cells_.erase(it);
            continue;
        }

        auto members=inputIt->second;
        std::sort(members.begin(),members.end(),[](const StoredSubmesh* a,const StoredSubmesh* b){
            if(a->key!=b->key)return a->key<b->key;
            if(a->entityId!=b->entityId)return a->entityId<b->entityId;
            return a->submeshSlot<b->submeshSlot;
        });

        std::vector<StaticBatchEntry> nextBatches;
        std::vector<CellBatchCpuGeometry> nextCpu;

        std::size_t cursor=0;
        while(cursor<members.size())
        {
            const StaticBatchKey groupKey=members[cursor]->key;
            std::size_t groupEnd=cursor+1;
            while(groupEnd<members.size()&&members[groupEnd]->key==groupKey)++groupEnd;

            CellBatchCpuGeometry geom;
            std::vector<StaticBatchSubmeshRange> ranges;
            ranges.reserve(groupEnd-cursor);
            Vec3 minB{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
            Vec3 maxB{-minB.x,-minB.y,-minB.z};

            std::size_t totalVerts=0,totalIndices=0;
            for(std::size_t m=cursor;m<groupEnd;++m)
            {
                totalVerts+=members[m]->worldVertices.size();
                totalIndices+=members[m]->localIndices.size();
            }
            if(totalVerts>0xFFFFFFFFull||totalIndices>0xFFFFFFFFull)return {rhi::Error::CapacityExceeded};
            geom.vertices.reserve(totalVerts);
            geom.indices.reserve(totalIndices);

            for(std::size_t m=cursor;m<groupEnd;++m)
            {
                const auto& sub=*members[m];
                const auto firstVertex=static_cast<std::uint32_t>(geom.vertices.size());
                const auto firstIndex=static_cast<std::uint32_t>(geom.indices.size());
                geom.vertices.insert(geom.vertices.end(),sub.worldVertices.begin(),sub.worldVertices.end());
                for(const auto localIdx:sub.localIndices)geom.indices.push_back(firstVertex+localIdx);
                minB={(std::min)(minB.x,sub.worldBounds.minimum.x),(std::min)(minB.y,sub.worldBounds.minimum.y),(std::min)(minB.z,sub.worldBounds.minimum.z)};
                maxB={(std::max)(maxB.x,sub.worldBounds.maximum.x),(std::max)(maxB.y,sub.worldBounds.maximum.y),(std::max)(maxB.z,sub.worldBounds.maximum.z)};
                ranges.push_back({firstIndex,static_cast<std::uint32_t>(sub.localIndices.size()),
                    firstVertex,static_cast<std::uint32_t>(sub.worldVertices.size()),
                    sub.entityId,sub.submeshSlot,sub.worldBounds});
            }

            const std::string tag="StaticBatch["+std::to_string(cellCoord.x)+","+std::to_string(cellCoord.y)+","+std::to_string(cellCoord.z)+"]";
            rhi::Buffer vb,ib;
            auto res=device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),tag+".VB"},
                std::as_bytes(std::span<const rhi::Vertex>(geom.vertices)),vb);
            if(!res){RefreshFlatView();return res;}
            res=device.CreateBuffer({rhi::BufferUsage::Index,sizeof(std::uint32_t),tag+".IB"},
                std::as_bytes(std::span<const std::uint32_t>(geom.indices)),ib);
            if(!res){RefreshFlatView();return res;}

            StaticBatchEntry entry;
            entry.cell=cellCoord;
            entry.key=groupKey;
            entry.vertices=std::move(vb);
            entry.indices=std::move(ib);
            entry.worldBounds={minB,maxB};
            entry.vertexCount=static_cast<std::uint32_t>(geom.vertices.size());
            entry.indexCount=static_cast<std::uint32_t>(geom.indices.size());
            entry.draw=members[cursor]->prototype;
            entry.draw.vertices=*entry.vertices;
            entry.draw.indices=*entry.indices;
            entry.draw.world=Mat4::Identity();
            entry.draw.hasPreviousWorld=true;
            entry.draw.previousWorld=Mat4::Identity();
            entry.draw.indexCount=entry.indexCount;
            entry.draw.worldBounds=entry.worldBounds;
            entry.draw.hasWorldBounds=true;
            entry.draw.staticShadowCaster=true;
            entry.draw.geometryClusters={};
            entry.ranges=std::move(ranges);

            nextBatches.push_back(std::move(entry));
            nextCpu.push_back(std::move(geom));
            cursor=groupEnd;
        }

        cell.batches=std::move(nextBatches);
        cell.cpuGeometry=std::move(nextCpu);
        cell.dirty=false;
        ++it;
    }

    stats_.entityCount=entities_.size();
    stats_.sourceSubmeshes=totalSubmeshes;
    stats_.lastRebuiltCells=rebuiltCells;
    stats_.lastCachedCells=cachedCells;
    RefreshFlatView();
    return {};
}

void StaticBatchCache::RefreshFlatView()
{
    flatBatches_.clear();
    flatCpuRefs_.clear();
    std::uint64_t vBytes=0,iBytes=0;
    for(const auto& [coord,cell]:cells_)
    {
        for(std::size_t i=0;i<cell.batches.size();++i)
        {
            const auto& b=cell.batches[i];
            vBytes+=static_cast<std::uint64_t>(b.vertexCount)*sizeof(rhi::Vertex);
            iBytes+=static_cast<std::uint64_t>(b.indexCount)*sizeof(std::uint32_t);
            flatBatches_.push_back(b);
            flatCpuRefs_.emplace_back(coord,i);
        }
    }
    stats_.activeCells=cells_.size();
    stats_.batchCount=flatBatches_.size();
    stats_.mergedVertexBytes=vBytes;
    stats_.mergedIndexBytes=iBytes;
}

StableEntityId StaticBatchCache::ResolveEntityByTriangle(std::size_t batchIndex,std::uint32_t triangleIndex,std::uint32_t* outSubmeshSlot) const noexcept
{
    if(batchIndex>=flatBatches_.size())return 0;
    const auto& batch=flatBatches_[batchIndex];
    const std::uint64_t targetIndex=static_cast<std::uint64_t>(triangleIndex)*3ull;
    if(targetIndex+2ull>=batch.indexCount)return 0;

    const auto it=std::upper_bound(batch.ranges.begin(),batch.ranges.end(),static_cast<std::uint32_t>(targetIndex),
        [](std::uint32_t idx,const StaticBatchSubmeshRange& r){ return idx<r.firstIndex; });
    if(it==batch.ranges.begin())return 0;
    const auto& found=*std::prev(it);
    if(targetIndex<found.firstIndex||targetIndex>=static_cast<std::uint64_t>(found.firstIndex)+found.indexCount)return 0;
    if(outSubmeshSlot)*outSubmeshSlot=found.submeshSlot;
    return found.entityId;
}

std::optional<StaticBatchPickHit> StaticBatchCache::RaycastPick(const Vec3& rayOrigin,const Vec3& rayDirection,float maxDistance) const
{
    if(!IsFinite(rayOrigin)||!IsFinite(rayDirection)||!(maxDistance>0.f)||!std::isfinite(maxDistance))return std::nullopt;
    const float dirLen=Length(rayDirection);
    if(dirLen<1e-8f)return std::nullopt;
    const Vec3 dir=rayDirection/dirLen;
    const Vec3 invDir{
        std::abs(dir.x)>1e-20f?(1.f/dir.x):1e24f,
        std::abs(dir.y)>1e-20f?(1.f/dir.y):1e24f,
        std::abs(dir.z)>1e-20f?(1.f/dir.z):1e24f
    };

    std::optional<StaticBatchPickHit> best;
    float closest=maxDistance;
    for(std::size_t bIdx=0;bIdx<flatBatches_.size();++bIdx)
    {
        const auto& batch=flatBatches_[bIdx];
        if(!RayIntersectsAabb(rayOrigin,invDir,batch.worldBounds,closest))continue;
        const auto& [cellCoord,localIdx]=flatCpuRefs_[bIdx];
        const auto cellIt=cells_.find(cellCoord);
        if(cellIt==cells_.end()||localIdx>=cellIt->second.cpuGeometry.size())continue;
        const auto& cpu=cellIt->second.cpuGeometry[localIdx];

        for(const auto& range:batch.ranges)
        {
            if(!RayIntersectsAabb(rayOrigin,invDir,range.worldBounds,closest))continue;
            const std::uint32_t endIdx=range.firstIndex+range.indexCount;
            for(std::uint32_t idx=range.firstIndex;idx+2<endIdx;idx+=3)
            {
                const auto& a=cpu.vertices[cpu.indices[idx]].position;
                const auto& b=cpu.vertices[cpu.indices[idx+1]].position;
                const auto& c=cpu.vertices[cpu.indices[idx+2]].position;
                float hitT=0.f;
                if(RayIntersectsTriangle(rayOrigin,dir,{a[0],a[1],a[2]},{b[0],b[1],b[2]},{c[0],c[1],c[2]},closest,hitT))
                {
                    closest=hitT;
                    best=StaticBatchPickHit{
                        range.entityId,
                        range.submeshSlot,
                        bIdx,
                        idx/3u,
                        hitT,
                        rayOrigin+dir*hitT
                    };
                }
            }
        }
    }
    return best;
}
}
