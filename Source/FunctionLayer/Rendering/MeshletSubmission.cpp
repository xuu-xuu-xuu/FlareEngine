#include "MeshletSubmission.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>
#include <utility>

namespace se
{
namespace
{
using Triangle=std::array<std::uint32_t,3>;
struct TriangleHash
{
    size_t operator()(const Triangle& value) const noexcept
    {
        size_t hash=0;
        for(const auto index:value)hash^=static_cast<size_t>(index)+0x9e3779b9u+(hash<<6)+(hash>>2);
        return hash;
    }
};
struct Occurrences {std::vector<std::uint32_t> triangles;size_t used=0;};
Vec3 Position(const MeshVertex& vertex){return {vertex.position[0],vertex.position[1],vertex.position[2]};}
bool Invalid(std::vector<AssetDiagnostic>& diagnostics,const char* detail)
{asset_io::Diagnostic(diagnostics,"Assets.MeshletInvalid",detail);return false;}
}
bool FlattenMeshletSubmission(const MeshData& source,const MeshletSet& meshlets,MeshletSubmission& output,
    std::vector<AssetDiagnostic>& diagnostics,std::uint32_t maximumRanges)
{
    if(!maximumRanges||maximumRanges>MaximumMeshletSubmissionRanges||source.vertices.empty()||source.indices.empty()
        ||source.indices.size()%3||source.vertices.size()>UINT32_MAX||source.indices.size()>UINT32_MAX
        ||source.skinned||!source.skinning.empty())return Invalid(diagnostics,"submission.source");
    const auto triangleCount=static_cast<std::uint32_t>(source.indices.size()/3);
    if(meshlets.sourceVertexCount!=source.vertices.size()||meshlets.triangleCount!=triangleCount
        ||meshlets.clusters.empty()||meshlets.clusters.size()>triangleCount)return Invalid(diagnostics,"submission.meshlets");
    for(const auto& vertex:source.vertices)if(!IsFinite(Position(vertex)))return Invalid(diagnostics,"submission.position");
    std::unordered_map<Triangle,Occurrences,TriangleHash> occurrences;
    occurrences.reserve(triangleCount);
    for(std::uint32_t triangle=0;triangle<triangleCount;++triangle)
    {
        Triangle key;
        for(size_t corner=0;corner<3;++corner)
        {
            key[corner]=source.indices[static_cast<size_t>(triangle)*3+corner];
            if(key[corner]>=source.vertices.size())return Invalid(diagnostics,"submission.sourceIndex");
        }
        occurrences[key].triangles.push_back(triangle);
    }
    std::vector<std::uint32_t> owners(triangleCount,UINT32_MAX);
    std::vector<Aabb> boxes;boxes.reserve(meshlets.clusters.size());
    std::uint32_t assigned=0;
    for(std::uint32_t clusterIndex=0;clusterIndex<meshlets.clusters.size();++clusterIndex)
    {
        const auto& cluster=meshlets.clusters[clusterIndex];
        if(cluster.vertices.empty()||cluster.vertices.size()>MeshletMaxVertices||cluster.indices.empty()
            ||cluster.indices.size()%3||cluster.indices.size()/3>MeshletMaxTriangles
            ||cluster.indices.size()/3>triangleCount-assigned)return Invalid(diagnostics,"submission.cluster");
        Aabb bounds;bool first=true;
        for(size_t local=0;local<cluster.vertices.size();++local)
        {
            const auto index=cluster.vertices[local];
            if(index>=source.vertices.size()
                ||std::find(cluster.vertices.begin(),cluster.vertices.begin()+local,index)!=cluster.vertices.begin()+local)
                return Invalid(diagnostics,"submission.vertexRemap");
            const auto position=Position(source.vertices[index]);
            if(first){bounds={position,position};first=false;}
            else
            {
                bounds.minimum={std::min(bounds.minimum.x,position.x),std::min(bounds.minimum.y,position.y),std::min(bounds.minimum.z,position.z)};
                bounds.maximum={std::max(bounds.maximum.x,position.x),std::max(bounds.maximum.y,position.y),std::max(bounds.maximum.z,position.z)};
            }
        }
        if(!IsValid(bounds))return Invalid(diagnostics,"submission.bounds");
        boxes.push_back(bounds);
        for(size_t offset=0;offset<cluster.indices.size();offset+=3)
        {
            Triangle key;
            for(size_t corner=0;corner<3;++corner)
            {
                const auto local=cluster.indices[offset+corner];
                if(local>=cluster.vertices.size())return Invalid(diagnostics,"submission.localIndex");
                key[corner]=cluster.vertices[local];
            }
            const auto found=occurrences.find(key);
            if(found==occurrences.end()||found->second.used>=found->second.triangles.size())
                return Invalid(diagnostics,"submission.coverage");
            auto& match=found->second;
            const auto triangle=match.triangles[match.used++];
            owners[triangle]=clusterIndex;++assigned;
        }
    }
    if(assigned!=triangleCount)return Invalid(diagnostics,"submission.missingTriangles");
    MeshletSubmission next;next.sourceVertexCount=static_cast<std::uint32_t>(source.vertices.size());
    next.triangleCount=triangleCount;next.clusterCount=static_cast<std::uint32_t>(meshlets.clusters.size());
    // 不按 cluster 重排 IB：相同深度的两片三角形仍按源次序竞争 LESS。
    next.indices=source.indices;
    for(std::uint32_t triangle=0;triangle<triangleCount;++triangle)
    {
        const auto owner=owners[triangle];
        if(owner>=boxes.size())return Invalid(diagnostics,"submission.unassigned");
        if(next.ranges.empty()||next.ranges.back().cluster!=owner)
        {
            if(next.ranges.size()>=maximumRanges)return Invalid(diagnostics,"submission.commandCapacity");
            next.ranges.push_back({owner,triangle*3,3,boxes[owner]});
        }
        else next.ranges.back().indexCount+=3;
    }
    output=std::move(next);return true;
}
bool BakeMeshletSubmission(const MeshData& source,MeshletSubmission& output,
    std::vector<AssetDiagnostic>& diagnostics,std::uint32_t maximumRanges)
{
    if(source.skinned||!source.skinning.empty()||!maximumRanges||maximumRanges>MaximumMeshletSubmissionRanges)
        return Invalid(diagnostics,"submission.unsupported");
    MeshletSet meshlets;
    if(!BakeMeshlets(source,meshlets,diagnostics))return false;
    return FlattenMeshletSubmission(source,meshlets,output,diagnostics,maximumRanges);
}
bool TransformMeshletSubmissionBounds(const MeshletSubmission& source,const Mat4& world,std::vector<Aabb>& output)
{
    if(!source.sourceVertexCount||!source.clusterCount||!source.triangleCount||source.ranges.empty()
        ||source.ranges.size()>MaximumMeshletSubmissionRanges||source.indices.size()!=std::uint64_t{source.triangleCount}*3
        ||!IsFinite(world))return false;
    std::vector<Aabb> next;next.reserve(source.ranges.size());
    size_t expectedFirst=0;
    for(const auto& range:source.ranges)
    {
        if(range.cluster>=source.clusterCount||range.firstIndex!=expectedFirst||!range.indexCount||range.indexCount%3
            ||range.firstIndex>source.indices.size()||range.indexCount>source.indices.size()-range.firstIndex)return false;
        const auto bounds=TransformBounds(range.localBounds,world);if(!bounds)return false;
        next.push_back(*bounds);expectedFirst+=range.indexCount;
    }
    if(expectedFirst!=source.indices.size())return false;
    output=std::move(next);return true;
}
}
