#include <metal_stdlib>
using namespace metal;
struct ClusterLight {float4 positionRange;float4 colorIntensity;float4 directionOuterCos;float4 coneShadow;};
struct ClusterParameters {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4 depth;uint4 dimensions;uint4 counts;};
kernel void clusteredLightingCompute(const device ClusterLight* clusterLights [[buffer(19)]],
    device uint* clusterList [[buffer(20)]],constant ClusterParameters& clusters [[buffer(21)]],
    uint cell [[thread_position_in_grid]])
{
    uint total=clusters.dimensions.x*clusters.dimensions.y*clusters.dimensions.z;
    if(cell>=total)return;
    uint3 index=uint3(cell%clusters.dimensions.x,(cell/clusters.dimensions.x)%clusters.dimensions.y,
        cell/(clusters.dimensions.x*clusters.dimensions.y));
    float2 lower=float2(index.xy)/float2(clusters.dimensions.xy)*2-1;
    float2 upper=float2(index.xy+uint2(1))/float2(clusters.dimensions.xy)*2-1;
    float begin=float(index.z)/float(clusters.dimensions.z),end=float(index.z+1)/float(clusters.dimensions.z);
    float nearDepth=clusters.counts.z!=0?clusters.depth.x*exp(clusters.depth.z*begin):mix(clusters.depth.x,clusters.depth.y,begin);
    float farDepth=clusters.counts.z!=0?clusters.depth.x*exp(clusters.depth.z*end):mix(clusters.depth.x,clusters.depth.y,end);
    float3 minimum=float3(1e30f),maximum=float3(-1e30f);
    for(uint corner=0;corner<8;++corner)
    {
        float2 xy=float2((corner&1u)!=0?upper.x:lower.x,(corner&2u)!=0?upper.y:lower.y);
        float4 projected=clusters.inverseProjection*float4(xy,0,1);
        float3 point=projected.xyz/projected.w;float distance=(corner&4u)!=0?farDepth:nearDepth;
        if(clusters.counts.z!=0)point*=distance/max(-point.z,1e-20f);else point.z=-distance;
        minimum=min(minimum,point);maximum=max(maximum,point);
    }
    uint base=cell*(clusters.dimensions.w+1u),count=0;
    for(uint lightIndex=0;lightIndex<clusters.counts.x;++lightIndex)
    {
        ClusterLight light=clusterLights[lightIndex];
        float3 center=(clusters.view*float4(light.positionRange.xyz,1)).xyz;
        float radius=light.positionRange.w*clusters.depth.w;
        float3 delta=center-clamp(center,minimum,maximum);
        if(dot(delta,delta)<=radius*radius)
        {
            // 셀 용량을 넘으면 전체 광원 순회로 전환한다.
            if(count==clusters.dimensions.w){clusterList[base]=0xffffffffu;return;}
            clusterList[base+1u+count++]=lightIndex;
        }
    }
    clusterList[base]=count;
}
