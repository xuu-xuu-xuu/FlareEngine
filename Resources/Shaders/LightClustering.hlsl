#define SE_CLUSTERED 1
#define SE_CLUSTER_BUILD 1
#include "ForwardBindings.hlsli"
#include "ClusteredBindings.hlsli"
RWStructuredBuffer<uint> buildCounts:register(u0);
RWStructuredBuffer<uint2> buildRanges:register(u1);
RWStructuredBuffer<uint> buildIndices:register(u2);
RWStructuredBuffer<uint> buildCounters:register(u3);

void ClusterBox(uint index,out float3 low,out float3 high)
{
    uint z=index/144,y=(index%144)/16,x=index%16;
    float d0=clusterParameters.x*pow(abs(clusterParameters.y/clusterParameters.x),z/24.0);
    float d1=clusterParameters.x*pow(abs(clusterParameters.y/clusterParameters.x),(z+1)/24.0);
    low=float3(1e30,1e30,1e30);high=-low;
    [unroll]for(uint dz=0;dz<2;++dz)[unroll]for(uint dy=0;dy<2;++dy)[unroll]for(uint dx=0;dx<2;++dx)
    {
        float depth=dz==0?d0:d1;
        float4 pz=mul(clusterProjection,float4(0,0,-depth,1));
        float4 corner=mul(clusterInverseProjection,float4(-1+2*(x+dx)/16.0,1-2*(y+dy)/9.0,pz.z/pz.w,1));
        float3 p=corner.xyz/corner.w;low=min(low,p);high=max(high,p);
    }
}
bool Intersects(uint light,float3 low,float3 high,out float distance)
{
    float4 data=clusterLights[light].positionRange;
    float3 center=mul(clusterView,float4(data.xyz,1)).xyz;
    float3 delta=center-clamp(center,low,high);
    float3 fromCenter=center-(low+high)*.5;distance=dot(fromCenter,fromCenter);
    return dot(delta,delta)<=data.w*data.w;
}
// counts 前 3456 项是公开 rawCount；后续每簇 8 个 uint 是内部候选掩码。
uint CandidateOffset(uint cluster,uint block){return 3456+cluster*8+block;}
groupshared float3 sharedLow,sharedHigh;
groupshared uint candidateMasks[8];
[numthreads(64,1,1)]
void CountLights(uint3 group:SV_GroupID,uint lane:SV_GroupIndex)
{
    uint index=group.x;
    if(index<3456)
    {
        if(lane==0)ClusterBox(index,sharedLow,sharedHigh);
        if(lane<8)candidateMasks[lane]=0;
        GroupMemoryBarrierWithGroupSync();
        [loop]for(uint light=lane;light<(uint)clusterParameters.z;light+=64)
        {
            float distance;
            if(Intersects(light,sharedLow,sharedHigh,distance))InterlockedOr(candidateMasks[light/32],1u<<(light%32));
        }
        GroupMemoryBarrierWithGroupSync();
        if(lane==0)
        {
            uint count=0;
            [unroll]for(uint block=0;block<8;++block)
            {
                uint mask=candidateMasks[block];count+=countbits(mask);
                buildCounts[CandidateOffset(index,block)]=mask;
            }
            buildCounts[index]=count;
        }
    }
}
groupshared uint scan[4096];
groupshared uint maxRaw,truncated,dropped,total;
groupshared uint3 metrics[256];
[numthreads(256,1,1)]
void PrefixLights(uint thread:SV_GroupIndex)
{
    uint localMax=0,localTruncated=0,localDropped=0;
    for(uint scanIndex=thread;scanIndex<4096;scanIndex+=256)
    {
        uint raw=0;if(scanIndex<3456)raw=buildCounts[scanIndex];scan[scanIndex]=min(raw,32);
        if(scanIndex<3456)
        {
            localMax=max(localMax,raw);
            if(raw>32){++localTruncated;localDropped+=raw-32;}
        }
    }
    metrics[thread]=uint3(localMax,localTruncated,localDropped);
    GroupMemoryBarrierWithGroupSync();
    [loop]for(uint upStride=1;upStride<4096;upStride*=2)
    {
        for(uint upIndex=(thread+1)*upStride*2-1;upIndex<4096;upIndex+=upStride*512)scan[upIndex]+=scan[upIndex-upStride];
        // 复用扫描的同步点归约计数，避免每簇争用同一 groupshared 原子计数器。
        if(upStride<256)
        {
            uint metricIndex=(thread+1)*upStride*2-1;
            if(metricIndex<256)
            {
                uint3 left=metrics[metricIndex-upStride],right=metrics[metricIndex];
                metrics[metricIndex]=uint3(max(left.x,right.x),left.yz+right.yz);
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(thread==0)
    {
        total=scan[4095];scan[4095]=0;
        maxRaw=metrics[255].x;truncated=metrics[255].y;dropped=metrics[255].z;
    }
    GroupMemoryBarrierWithGroupSync();
    [loop]for(uint downStride=2048;downStride>0;downStride/=2)
    {
        for(uint downIndex=(thread+1)*downStride*2-1;downIndex<4096;downIndex+=downStride*512)
        {uint previous=scan[downIndex-downStride];scan[downIndex-downStride]=scan[downIndex];scan[downIndex]+=previous;}
        GroupMemoryBarrierWithGroupSync();
    }
    bool overflow=total>(uint)clusterParameters.w;
    for(uint rangeIndex=thread;rangeIndex<3456;rangeIndex+=256)buildRanges[rangeIndex]=overflow?uint2(0,0):uint2(scan[rangeIndex],min(buildCounts[rangeIndex],32));
    if(thread==0)
    {
        buildCounters[0]=(uint)clusterParameters.z;buildCounters[1]=maxRaw;buildCounters[2]=truncated;buildCounters[3]=dropped;
        buildCounters[4]=total;buildCounters[5]=(uint)clusterParameters.w;buildCounters[6]=overflow?1:0;
        buildCounters[7]=overflow?min(8,(uint)clusterParameters.z):0;
    }
}
// 稀疏与截断填表使用独立 PSO，避免常见路径承担 32 项局部数组的寄存器压力。
[numthreads(64,1,1)]
void FillSparseLights(uint3 thread:SV_DispatchThreadID)
{
    uint index=thread.x;
    if(index<3456&&buildCounters[6]==0&&buildCounts[index]<=32)
    {
        uint2 range=buildRanges[index];uint written=0;
        [unroll]for(uint block=0;block<8;++block)
        {
            uint mask=buildCounts[CandidateOffset(index,block)];
            [loop]while(mask!=0)
            {
                uint bit=(uint)firstbitlow(mask);
                buildIndices[range.x+written++]=block*32+bit;mask&=mask-1;
            }
        }
    }
}
[numthreads(64,1,1)]
void FillDenseLights(uint3 thread:SV_DispatchThreadID)
{
    uint index=thread.x;
    if(index<3456&&buildCounters[6]==0&&buildCounts[index]>32)
    {
        float3 low,high;ClusterBox(index,low,high);float3 clusterCenter=(low+high)*.5;
        uint selected[32];float distances[32];
        [unroll]for(uint k=0;k<32;++k){selected[k]=0;distances[k]=0;}
        uint count=0;
        [loop]for(uint block=0;block<8;++block)
        {
            uint mask=buildCounts[CandidateOffset(index,block)];
            [loop]while(mask!=0)
            {
                uint bit=(uint)firstbitlow(mask),light=block*32+bit;mask&=mask-1;
                float3 lightCenter=mul(clusterView,float4(clusterLights[light].positionRange.xyz,1)).xyz;
                float3 delta=lightCenter-clusterCenter;float distance=dot(delta,delta);
                if(count<32){selected[count]=light;distances[count]=distance;++count;}
                else
                {
                    uint worst=0;
                    [unroll]for(uint n=1;n<32;++n)
                        if(distances[n]>distances[worst]||(distances[n]==distances[worst]&&selected[n]>selected[worst]))worst=n;
                    if(distance<distances[worst]||(distance==distances[worst]&&light<selected[worst]))
                    {selected[worst]=light;distances[worst]=distance;}
                }
            }
        }
        [loop]for(uint a=0;a<count;++a)[loop]for(uint b=a+1;b<count;++b)
            if(selected[b]<selected[a]){uint value=selected[a];selected[a]=selected[b];selected[b]=value;}
        uint2 range=buildRanges[index];
        [loop]for(uint slot=0;slot<count;++slot)buildIndices[range.x+slot]=selected[slot];
    }
}
